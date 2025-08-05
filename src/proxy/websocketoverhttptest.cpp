/*
 * Copyright (C) 2025 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $FANOUT_END_LICENSE$
 */

#include <unordered_map>
#include <QDir>
#include <boost/signals2.hpp>
#include "test.h"
#include "log.h"
#include "zhttpmanager.h"
#include "websocketoverhttp.h"

namespace {

class WohServer
{
public:
	std::unique_ptr<ZhttpManager> zhttpIn;
	std::unordered_map<ZhttpRequest*, std::unique_ptr<ZhttpRequest>> reqs;

	WohServer(const QDir &workDir)
	{
		zhttpIn = std::make_unique<ZhttpManager>();
		zhttpIn->setInstanceId("woh-test-server");
		zhttpIn->setBind(true);
		zhttpIn->setServerInSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("woh-test-in")));
		zhttpIn->setServerInStreamSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("woh-test-in-stream")));
		zhttpIn->setServerOutSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("woh-test-out")));
		zhttpIn->requestReady.connect(boost::bind(&WohServer::zhttpIn_requestReady, this));
	}

	void zhttpIn_requestReady()
	{
		ZhttpRequest *req = zhttpIn->takeNextRequest();
		if(!req)
			return;

		req->readyRead.connect(boost::bind(&WohServer::req_readyRead, this, req));
		req->bytesWritten.connect(boost::bind(&WohServer::req_bytesWritten, this, req, boost::placeholders::_1));

		reqs.emplace(std::make_pair(req, std::unique_ptr<ZhttpRequest>(req)));

		req_readyRead(req);
	}

	void req_readyRead(ZhttpRequest *req)
	{
		if(!req->isInputFinished())
			return;

		handle(req);
	}

	void req_bytesWritten(ZhttpRequest *req, int written)
	{
		Q_UNUSED(written);

		if(!req->isFinished())
			return;

		reqs.erase(req);
	}

	void respond(ZhttpRequest *req, int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		req->beginResponse(code, reason, headers);
		req->writeBody(body);
		req->endBody();
	}

	void respondOk(ZhttpRequest *req, const QByteArray &body, int accepted = -1)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "application/websocket-events");
		if(accepted >= 0)
			headers += HttpHeader("Content-Bytes-Accepted", QByteArray::number(accepted));

		respond(req, 200, "OK", headers, body);
	}

	void respondError(ZhttpRequest *req, int code, const QByteArray &reason, const QByteArray &body)
	{
		respond(req, code, reason, HttpHeaders(), body);
	}

	void handle(ZhttpRequest *req)
	{
		if(req->requestMethod() != "POST")
		{
			respondError(req, 400, "Bad Request", "Method must be POST\n");
			return;
		}

		QUrl uri = req->requestUri();
		HttpHeaders headers = req->requestHeaders();
		QByteArray body = req->readBody();

		if(headers.get("Content-Type") != "application/websocket-events")
		{
			respondError(req, 400, "Bad Request", "Content-Type must be application/websocket-events\n");
			return;
		}

		if(headers.get("Accept") != "application/websocket-events")
		{
			respondError(req, 400, "Bad Request", "Accept must be application/websocket-events\n");
			return;
		}

		if(uri.path() == "/ws")
		{
			if(body == "OPEN\r\n")
				respondOk(req, "OPEN\r\n");
			else if(body == "TEXT 5\r\nhello\r\n")
				respondOk(req, "TEXT 5\r\nworld\r\n");
			else if(body == "TEXT b\r\n[foo][hello\r\n")
				respondOk(req, "", 5);
			else if(body == "TEXT 6\r\n[hello\r\nTEXT 7\r\n world]\r\n")
			{
				if(headers.get("Content-Bytes-Replayed") != "6")
				{
					respondError(req, 400, "Bad Request", "Expected replayed=5\n");
					return;
				}

				respondOk(req, "TEXT 4\r\n[ok]\r\n");
			}
			else if(body == "CLOSE\r\n")
				respondOk(req, "CLOSE\r\n");
			else
				respondOk(req, "");
		}
		else
		{
			respondError(req, 404, "Not Found", "Not Found\n");
		}
	}
};

class TestState
{
public:
	std::unique_ptr<WohServer> wohServer;
	std::unique_ptr<ZhttpManager> zhttpOut;

	TestState(std::function<void (int)> loop_wait)
	{
		log_setOutputLevel(LOG_LEVEL_WARNING);

		QDir outDir(qgetenv("OUT_DIR"));
		QDir workDir(QDir::current().relativeFilePath(outDir.filePath("test-work")));

		wohServer = std::make_unique<WohServer>(workDir);

		zhttpOut = std::make_unique<ZhttpManager>();
		zhttpOut->setInstanceId("woh-test-client");
		zhttpOut->setClientOutSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("woh-test-in")));
		zhttpOut->setClientOutStreamSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("woh-test-in-stream")));
		zhttpOut->setClientInSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("woh-test-out")));

		loop_wait(500);
	}

	~TestState()
	{
		WebSocketOverHttp::clearDisconnectManager();

		zhttpOut.reset();
		wohServer.reset();
	}
};

}

static void convertFrames()
{
	QList<WebSocket::Frame> frames;
	frames += WebSocket::Frame(WebSocket::Frame::Text, "hello", true);
	frames += WebSocket::Frame(WebSocket::Frame::Continuation, " world", false);

	bool ok = false;
	int framesRepresented = 0;
	int contentRepresented = 0;
	QList<WebSocketOverHttp::Event> events = WebSocketOverHttp::framesToEvents(frames, -1, -1, &ok, &framesRepresented, &contentRepresented);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(events.count(), 1);
	TEST_ASSERT_EQ(events[0].type, "TEXT");
	TEST_ASSERT_EQ(events[0].content, "hello world");

	int removed = WebSocketOverHttp::removeContentFromFrames(&frames, contentRepresented);
	TEST_ASSERT_EQ(removed, contentRepresented);
	TEST_ASSERT(frames.isEmpty());
}

static void removePartial()
{
	QList<WebSocket::Frame> frames;
	frames += WebSocket::Frame(WebSocket::Frame::Text, "hello", true);
	frames += WebSocket::Frame(WebSocket::Frame::Continuation, " world", false);
	frames += WebSocket::Frame(WebSocket::Frame::Text, "", false);
	frames += WebSocket::Frame(WebSocket::Frame::Text, "foo", false);

	int removed = WebSocketOverHttp::removeContentFromFrames(&frames, 3);
	TEST_ASSERT_EQ(removed, 3);
	TEST_ASSERT_EQ(frames.count(), 4);
	TEST_ASSERT_EQ(frames[0].type, WebSocket::Frame::Text);
	TEST_ASSERT_EQ(frames[0].data, "lo");

	removed = WebSocketOverHttp::removeContentFromFrames(&frames, 2);
	TEST_ASSERT_EQ(removed, 2);
	TEST_ASSERT_EQ(frames.count(), 3);
	TEST_ASSERT_EQ(frames[0].type, WebSocket::Frame::Text);
	TEST_ASSERT_EQ(frames[0].data, " world");

	removed = WebSocketOverHttp::removeContentFromFrames(&frames, 6);
	TEST_ASSERT_EQ(removed, 6);
	TEST_ASSERT_EQ(frames.count(), 1);
	TEST_ASSERT_EQ(frames[0].type, WebSocket::Frame::Text);
	TEST_ASSERT_EQ(frames[0].data, "foo");

	removed = WebSocketOverHttp::removeContentFromFrames(&frames, 10);
	TEST_ASSERT_EQ(removed, 3);
	TEST_ASSERT(frames.isEmpty());
}

static void io(std::function<void (int)> loop_wait)
{
	TestState state(loop_wait);

	WebSocketOverHttp client(state.zhttpOut.get());

	bool clientConnected = false;
	client.connected.connect([&] {
		clientConnected = true;
	});

	bool clientReadyRead = false;
	client.readyRead.connect([&] {
		clientReadyRead = true;
	});

	int clientFramesWritten = 0;
	int clientContentWritten = 0;
	client.framesWritten.connect([&](int count, int contentBytes) {
		clientFramesWritten += count;
		clientContentWritten += contentBytes;
	});

	bool clientClosed = false;
	client.closed.connect([&] {
		clientClosed = true;
	});

	bool clientError = false;
	client.error.connect([&] {
		clientError = true;
	});

	client.start(QUrl("ws://localhost/ws"), HttpHeaders());

	while(!clientConnected && !clientError)
		loop_wait(10);

	TEST_ASSERT(!clientError);
	TEST_ASSERT(clientConnected);

	client.writeFrame(WebSocket::Frame(WebSocket::Frame::Text, "hello", false));

	while(!clientReadyRead && !clientError)
		loop_wait(10);

	TEST_ASSERT(!clientError);
	TEST_ASSERT(clientReadyRead);
	TEST_ASSERT_EQ(clientFramesWritten, 1);
	TEST_ASSERT_EQ(clientContentWritten, 5);

	WebSocket::Frame f = client.readFrame();
	TEST_ASSERT_EQ(f.type, WebSocket::Frame::Text);
	TEST_ASSERT_EQ(f.data, "world");
	TEST_ASSERT(!f.more);

	client.close();

	while(!clientClosed && !clientError)
		loop_wait(10);

	TEST_ASSERT(!clientError);
	TEST_ASSERT(clientClosed);
}

static void replay(std::function<void (int)> loop_wait)
{
	TestState state(loop_wait);

	WebSocketOverHttp client(state.zhttpOut.get());

	bool clientConnected = false;
	client.connected.connect([&] {
		clientConnected = true;
	});

	bool clientReadyRead = false;
	client.readyRead.connect([&] {
		clientReadyRead = true;
	});

	int clientFramesWritten = 0;
	int clientContentWritten = 0;
	client.framesWritten.connect([&](int count, int contentBytes) {
		clientFramesWritten += count;
		clientContentWritten += contentBytes;
	});

	bool clientWriteBytesChanged = false;
	client.writeBytesChanged.connect([&] {
		clientWriteBytesChanged = true;
	});

	bool clientClosed = false;
	client.closed.connect([&] {
		clientClosed = true;
	});

	bool clientError = false;
	client.error.connect([&] {
		clientError = true;
	});

	client.start(QUrl("ws://localhost/ws"), HttpHeaders());

	while(!clientConnected && !clientError)
		loop_wait(10);

	TEST_ASSERT(!clientError);
	TEST_ASSERT(clientConnected);

	int maxAvail = client.writeBytesAvailable();
	client.writeFrame(WebSocket::Frame(WebSocket::Frame::Text, "[foo][hello", false));
	TEST_ASSERT_EQ(client.writeBytesAvailable(), maxAvail - 11);

	while(!clientWriteBytesChanged && !clientError)
		loop_wait(10);

	TEST_ASSERT(!clientError);
	TEST_ASSERT(clientWriteBytesChanged);
	TEST_ASSERT_EQ(clientFramesWritten, 0);
	TEST_ASSERT_EQ(clientContentWritten, 5);
	TEST_ASSERT_EQ(client.writeBytesAvailable(), maxAvail - 6);

	client.writeFrame(WebSocket::Frame(WebSocket::Frame::Text, " world]", false));

	while(!clientReadyRead && !clientError)
		loop_wait(10);

	WebSocket::Frame f = client.readFrame();
	TEST_ASSERT_EQ(f.type, WebSocket::Frame::Text);
	TEST_ASSERT_EQ(f.data, "[ok]");
	TEST_ASSERT(!f.more);
	TEST_ASSERT_EQ(clientFramesWritten, 2);
	TEST_ASSERT_EQ(clientContentWritten, 18);
	TEST_ASSERT_EQ(client.writeBytesAvailable(), maxAvail);

	client.close();

	while(!clientClosed && !clientError)
		loop_wait(10);

	TEST_ASSERT(!clientError);
	TEST_ASSERT(clientClosed);
}

extern "C" int websocketoverhttp_test(ffi::TestException *out_ex)
{
	TEST_CATCH(convertFrames());
	TEST_CATCH(removePartial());
	TEST_CATCH(test_with_event_loop(io));
	TEST_CATCH(test_with_event_loop(replay));

	return 0;
}
