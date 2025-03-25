/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#include <QDir>
#include "test.h"
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "qzmqreqmessage.h"
#include "qtcompat.h"
#include "log.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "packet/httpresponsedata.h"
#include "timer.h"
#include "defercall.h"
#include "handlerengine.h"

namespace {

class Wrapper
{
public:
	std::unique_ptr<QZmq::Socket> zhttpClientOutStreamSock;
	std::unique_ptr<QZmq::Socket> zhttpClientInSock;
	std::unique_ptr<QZmq::Valve> zhttpClientInValve;
	std::unique_ptr<QZmq::Socket> zhttpServerInSock;
	std::unique_ptr<QZmq::Valve> zhttpServerInValve;
	std::unique_ptr<QZmq::Socket> zhttpServerInStreamSock;
	std::unique_ptr<QZmq::Valve> zhttpServerInStreamValve;
	std::unique_ptr<QZmq::Socket> zhttpServerOutSock;
	std::unique_ptr<QZmq::Socket> proxyAcceptSock;
	std::unique_ptr<QZmq::Valve> proxyAcceptValve;
	std::unique_ptr<QZmq::Socket> publishPushSock;

	QDir workDir;
	bool acceptSuccess;
	QVariantHash acceptValue;
	bool finished;
	QHash<QByteArray, HttpResponseData> responses;
	int serverReqs;
	bool serverFailed;
	int serverOutSeq;
	QByteArray requestBody;
	Connection zhttpClientInValveConnection;
	Connection zhttpServerInValveConnection;
	Connection zhttpServerInStreamValveConnection;
	Connection proxyAcceptValveConnection;

	Wrapper(QDir _workDir) :
		workDir(_workDir),
		acceptSuccess(false),
		finished(false),
		serverReqs(0),
		serverFailed(false),
		serverOutSeq(0)
	{
		// http sockets

		zhttpClientOutStreamSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);

		zhttpClientInSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Sub);
		zhttpClientInValve = std::make_unique<QZmq::Valve>(zhttpClientInSock.get());
		zhttpClientInValveConnection = zhttpClientInValve->readyRead.connect(boost::bind(&Wrapper::zhttpClientIn_readyRead, this, boost::placeholders::_1));

		zhttpServerInSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Pull);
		zhttpServerInValve = std::make_unique<QZmq::Valve>(zhttpServerInSock.get());
		zhttpServerInValveConnection = zhttpServerInValve->readyRead.connect(boost::bind(&Wrapper::zhttpServerIn_readyRead, this, boost::placeholders::_1));

		zhttpServerInStreamSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);
		zhttpServerInStreamSock->setIdentity("test-server");
		zhttpServerInStreamValve = std::make_unique<QZmq::Valve>(zhttpServerInStreamSock.get());
		zhttpServerInStreamValveConnection = zhttpServerInStreamValve->readyRead.connect(boost::bind(&Wrapper::zhttpServerInStream_readyRead, this, boost::placeholders::_1));

		zhttpServerOutSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Pub);

		// proxy sockets

		proxyAcceptSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Dealer);
		proxyAcceptValve = std::make_unique<QZmq::Valve>(proxyAcceptSock.get());
		proxyAcceptValveConnection = proxyAcceptValve->readyRead.connect(boost::bind(&Wrapper::proxyAccept_readyRead, this, boost::placeholders::_1));

		// publish sockets

		publishPushSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Push);
	}

	void startHttp()
	{
		zhttpClientOutStreamSock->bind("ipc://" + workDir.filePath("client-out-stream"));
		zhttpClientInSock->bind("ipc://" + workDir.filePath("client-in"));
		zhttpServerInSock->bind("ipc://" + workDir.filePath("server-in"));
		zhttpServerInStreamSock->bind("ipc://" + workDir.filePath("server-in-stream"));
		zhttpServerOutSock->bind("ipc://" + workDir.filePath("server-out"));

		zhttpClientInSock->subscribe("test-client ");

		zhttpClientInValve->open();
		zhttpServerInValve->open();
		zhttpServerInStreamValve->open();
	}

	void startProxy()
	{
		proxyAcceptSock->bind("ipc://" + workDir.filePath("accept"));

		proxyAcceptValve->open();
	}

	void startPublish()
	{
		publishPushSock->connectToAddress("ipc://" + workDir.filePath("publish-pull"));
	}

	void reset()
	{
		acceptSuccess = false;
		acceptValue.clear();
		finished = false;
		responses.clear();
		serverReqs = 0;
		serverFailed = false;
		serverOutSeq = 0;
		requestBody.clear();
	}

	void zhttpClientIn_readyRead(const QList<QByteArray> &message)
	{
		log_debug("client in");

		int at = message[0].indexOf(' ');
		QVariant v = TnetString::toVariant(message[0].mid(at + 2));
		ZhttpResponsePacket zresp;
		zresp.fromVariant(v);
		if(zresp.type == ZhttpResponsePacket::Data)
		{
			if(!responses.contains(zresp.ids.first().id))
			{
				HttpResponseData rd;
				rd.code = zresp.code;
				rd.reason = zresp.reason;
				rd.headers = zresp.headers;
				responses[zresp.ids.first().id] = rd;
			}

			responses[zresp.ids.first().id].body += zresp.body;

			if(!zresp.more)
				finished = true;
		}
	}

	void zhttpServerIn_readyRead(const QList<QByteArray> &message)
	{
		++serverReqs;
		log_debug("server in");
		QVariant v = TnetString::toVariant(message[0].mid(1));
		ZhttpRequestPacket zreq;
		zreq.fromVariant(v);

		handleServerIn(zreq);
	}

	void zhttpServerInStream_readyRead(const QList<QByteArray> &message)
	{
		log_debug("server stream in");
		QVariant v = TnetString::toVariant(message[2].mid(1));
		ZhttpRequestPacket zreq;
		zreq.fromVariant(v);

		handleServerIn(zreq);
	}

	void handleServerIn(const ZhttpRequestPacket &zreq)
	{
		if(zreq.type == ZhttpRequestPacket::Cancel)
		{
			serverFailed = true;
			return;
		}

		if(zreq.type == ZhttpRequestPacket::Data)
			requestBody += zreq.body;

		if(zreq.more)
		{
			// ack
			if(serverOutSeq == 0)
			{
				ZhttpResponsePacket zresp;
				zresp.from = "test-server";
				zresp.ids += ZhttpResponsePacket::Id(zreq.ids.first().id, serverOutSeq++);
				zresp.type = ZhttpResponsePacket::Credit;
				zresp.credits = 200000;
				QByteArray buf = zreq.from + " T" + TnetString::fromVariant(zresp.toVariant());
				zhttpServerOutSock->write(QList<QByteArray>() << buf);
			}

			return;
		}

		ZhttpResponsePacket zresp;
		zresp.from = "test-server";
		zresp.ids += ZhttpResponsePacket::Id(zreq.ids.first().id, serverOutSeq++);
		zresp.type = ZhttpResponsePacket::Data;
		zresp.code = 200;
		zresp.reason = "OK";

		QByteArray encPath = zreq.uri.path(QUrl::FullyEncoded).toUtf8();

		zresp.headers += HttpHeader("Content-Type", "text/plain");
		zresp.body = "this is what's next\n";

		zresp.headers += HttpHeader("Content-Length", QByteArray::number(zresp.body.size()));
		QByteArray buf = zreq.from + " T" + TnetString::fromVariant(zresp.toVariant());
		zhttpServerOutSock->write(QList<QByteArray>() << buf);

		// zero out so we can accept another request
		serverOutSeq = 0;
	}

	void proxyAccept_readyRead(const QList<QByteArray> &_message)
	{
		QZmq::ReqMessage message(_message);
		QVariant v = TnetString::toVariant(message.content()[0]);

		TEST_ASSERT(typeId(v) == QMetaType::QVariantHash);
		QVariantHash vresp = v.toHash();

		TEST_ASSERT(vresp.value("success").toBool());

		acceptSuccess = true;

		v = vresp.value("value");
		TEST_ASSERT(typeId(v) == QMetaType::QVariantHash);
		acceptValue = v.toHash();
	}
};

class TestState
{
public:
	HandlerEngine *engine;
	Wrapper *wrapper;

	TestState()
	{
		log_setOutputLevel(LOG_LEVEL_WARNING);
		//log_setOutputLevel(LOG_LEVEL_DEBUG);

		QDir outDir(qgetenv("OUT_DIR"));
		QDir workDir(QDir::current().relativeFilePath(outDir.filePath("test-work")));

		Timer::init(100);

		wrapper = new Wrapper(workDir);
		wrapper->startHttp();
		wrapper->startProxy();

		engine = new HandlerEngine;

		HandlerEngine::Configuration config;
		config.instanceId = "handler";
		config.serverInStreamSpecs = QStringList() << ("ipc://" + workDir.filePath("client-out-stream"));
		config.serverOutSpecs = QStringList() << ("ipc://" + workDir.filePath("client-in"));
		config.clientOutSpecs = QStringList() << ("ipc://" + workDir.filePath("server-in"));
		config.clientOutStreamSpecs = QStringList() << ("ipc://" + workDir.filePath("server-in-stream"));
		config.clientInSpecs = QStringList() << ("ipc://" + workDir.filePath("server-out"));
		config.acceptSpecs = QStringList() << ("ipc://" + workDir.filePath("accept"));
		config.pushInSpec = ("ipc://" + workDir.filePath("publish-pull"));
		config.connectionSubscriptionMax = 20;
		config.connectionsMax = 20;
		TEST_ASSERT(engine->start(config));

		wrapper->startPublish();

		QTest::qWait(500);
	}

	~TestState()
	{
		delete engine;
		delete wrapper;

		// ensure deferred deletes are processed
		QCoreApplication::instance()->sendPostedEvents();

		DeferCall::cleanup();
		Timer::deinit();
	}
};

}

static void acceptNoHold()
{
	TestQCoreApplication qapp;
	TestState state;
	Wrapper *wrapper = state.wrapper;

	wrapper->reset();

	QByteArray id = "1";

	QVariantHash rid;
	rid["sender"] = QByteArray("test-client");
	rid["id"] = id;

	QVariantHash reqState;
	reqState["rid"] = rid;
	reqState["in-seq"] = 1;
	reqState["out-seq"] = 1;
	reqState["out-credits"] = 1000;

	QVariantHash req;
	req["method"] = QByteArray("GET");
	req["uri"] = QByteArray("http://example.com/path");
	QVariantList reqHeaders;
	req["headers"] = reqHeaders;
	req["body"] = QByteArray();

	QVariantHash resp;
	resp["code"] = 200;
	resp["reason"] = QByteArray("OK");
	QVariantList respHeaders;
	respHeaders += QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
	resp["headers"] = respHeaders;
	resp["body"] = QByteArray("hello world\n");

	QVariantHash args;
	args["requests"] = QVariantList() << reqState;
	args["request-data"] = req;
	args["orig-request-data"] = req;
	args["response"] = resp;

	QVariantHash data;
	data["id"] = id;
	data["method"] = QByteArray("accept");
	data["args"] = args;

	QByteArray buf = TnetString::fromVariant(data);
	wrapper->proxyAcceptSock->write(QList<QByteArray>() << QByteArray() << buf);
	while(!wrapper->acceptSuccess)
		QTest::qWait(10);

	TEST_ASSERT(!wrapper->acceptValue.value("accepted").toBool());
	TEST_ASSERT_EQ(wrapper->acceptValue["response"].toHash()["body"].toByteArray(), QByteArray("hello world\n"));
}

static void acceptNoHoldResponseSent()
{
	TestQCoreApplication qapp;
	TestState state;
	Wrapper *wrapper = state.wrapper;

	wrapper->reset();

	QByteArray id = "2";

	QVariantHash rid;
	rid["sender"] = QByteArray("test-client");
	rid["id"] = id;

	QVariantHash reqState;
	reqState["rid"] = rid;
	reqState["in-seq"] = 1;
	reqState["out-seq"] = 1;
	reqState["out-credits"] = 1000;

	QVariantHash req;
	req["method"] = QByteArray("GET");
	req["uri"] = QByteArray("http://example.com/path");
	QVariantList reqHeaders;
	req["headers"] = reqHeaders;
	req["body"] = QByteArray();

	QVariantHash resp;
	resp["code"] = 200;
	resp["reason"] = QByteArray("OK");
	QVariantList respHeaders;
	respHeaders += QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
	resp["headers"] = respHeaders;
	resp["body"] = QByteArray("hello world\n");

	QVariantHash args;
	args["requests"] = QVariantList() << reqState;
	args["request-data"] = req;
	args["orig-request-data"] = req;
	args["response"] = resp;
	args["response-sent"] = true;

	QVariantHash data;
	data["id"] = id;
	data["method"] = QByteArray("accept");
	data["args"] = args;

	QByteArray buf = TnetString::fromVariant(data);
	wrapper->proxyAcceptSock->write(QList<QByteArray>() << QByteArray() << buf);
	while(!wrapper->acceptSuccess)
		QTest::qWait(10);

	TEST_ASSERT(!wrapper->acceptValue.value("accepted").toBool());
	TEST_ASSERT(!wrapper->acceptValue.contains("response"));
}

static void acceptNoHoldNext()
{
	TestQCoreApplication qapp;
	TestState state;
	Wrapper *wrapper = state.wrapper;

	wrapper->reset();

	QByteArray id = "3";

	QVariantHash rid;
	rid["sender"] = QByteArray("test-client");
	rid["id"] = id;

	QVariantHash reqState;
	reqState["rid"] = rid;
	reqState["in-seq"] = 1;
	reqState["out-seq"] = 1;
	reqState["out-credits"] = 1000;

	QVariantHash req;
	req["method"] = QByteArray("GET");
	req["uri"] = QByteArray("http://example.com/path");
	QVariantList reqHeaders;
	req["headers"] = reqHeaders;
	req["body"] = QByteArray();

	QVariantHash resp;
	resp["code"] = 200;
	resp["reason"] = QByteArray("OK");
	QVariantList respHeaders;
	respHeaders += QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
	respHeaders += QVariant(QVariantList() << QByteArray("Grip-Link") << QByteArray("</next>; rel=next"));
	resp["headers"] = respHeaders;
	resp["body"] = QByteArray("hello world\n");

	QVariantHash args;
	args["requests"] = QVariantList() << reqState;
	args["request-data"] = req;
	args["orig-request-data"] = req;
	args["response"] = resp;

	QVariantHash data;
	data["id"] = id;
	data["method"] = QByteArray("accept");
	data["args"] = args;

	QByteArray buf = TnetString::fromVariant(data);
	wrapper->proxyAcceptSock->write(QList<QByteArray>() << QByteArray() << buf);
	while(!wrapper->acceptSuccess)
		QTest::qWait(10);

	TEST_ASSERT(wrapper->acceptValue.value("accepted").toBool());

	while(!wrapper->finished)
		QTest::qWait(10);

	TEST_ASSERT(wrapper->responses.contains(id));
	TEST_ASSERT_EQ(wrapper->responses.value(id).body, QByteArray("hello world\nthis is what's next\n"));
}

static void acceptNoHoldNextResponseSent()
{
	TestQCoreApplication qapp;
	TestState state;
	Wrapper *wrapper = state.wrapper;

	wrapper->reset();

	QByteArray id = "4";

	QVariantHash rid;
	rid["sender"] = QByteArray("test-client");
	rid["id"] = id;

	QVariantHash reqState;
	reqState["rid"] = rid;
	reqState["in-seq"] = 1;
	reqState["out-seq"] = 1;
	reqState["out-credits"] = 1000;
	reqState["response-code"] = 200;

	QVariantHash req;
	req["method"] = QByteArray("GET");
	req["uri"] = QByteArray("http://example.com/path");
	QVariantList reqHeaders;
	req["headers"] = reqHeaders;
	req["body"] = QByteArray();

	QVariantHash resp;
	resp["code"] = 200;
	resp["reason"] = QByteArray("OK");
	QVariantList respHeaders;
	respHeaders += QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
	respHeaders += QVariant(QVariantList() << QByteArray("Grip-Link") << QByteArray("</next>; rel=next"));
	resp["headers"] = respHeaders;
	resp["body"] = QByteArray("hello world\n");

	QVariantHash args;
	args["requests"] = QVariantList() << reqState;
	args["request-data"] = req;
	args["orig-request-data"] = req;
	args["response"] = resp;
	args["response-sent"] = true;

	QVariantHash data;
	data["id"] = id;
	data["method"] = QByteArray("accept");
	data["args"] = args;

	QByteArray buf = TnetString::fromVariant(data);
	wrapper->proxyAcceptSock->write(QList<QByteArray>() << QByteArray() << buf);
	while(!wrapper->acceptSuccess)
		QTest::qWait(10);

	TEST_ASSERT(wrapper->acceptValue.value("accepted").toBool());

	while(!wrapper->finished)
		QTest::qWait(10);

	TEST_ASSERT(wrapper->responses.contains(id));
	TEST_ASSERT_EQ(wrapper->responses.value(id).body, QByteArray("this is what's next\n"));
}

static void publishResponse()
{
	TestQCoreApplication qapp;
	TestState state;
	Wrapper *wrapper = state.wrapper;

	wrapper->reset();

	QByteArray id = "5";

	QVariantHash rid;
	rid["sender"] = QByteArray("test-client");
	rid["id"] = id;

	QVariantHash reqState;
	reqState["rid"] = rid;
	reqState["in-seq"] = 1;
	reqState["out-seq"] = 1;
	reqState["out-credits"] = 1000;

	QVariantHash req;
	req["method"] = QByteArray("GET");
	req["uri"] = QByteArray("http://example.com/path");
	QVariantList reqHeaders;
	req["headers"] = reqHeaders;
	req["body"] = QByteArray();

	QVariantHash resp;
	resp["code"] = 200;
	resp["reason"] = QByteArray("OK");
	QVariantList respHeaders;
	respHeaders += QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
	respHeaders += QVariant(QVariantList() << QByteArray("Grip-Hold") << QByteArray("response"));
	respHeaders += QVariant(QVariantList() << QByteArray("Grip-Channel") << QByteArray("apple"));
	resp["headers"] = respHeaders;
	resp["body"] = QByteArray("timeout\n");

	QVariantHash args;
	args["requests"] = QVariantList() << reqState;
	args["request-data"] = req;
	args["orig-request-data"] = req;
	args["response"] = resp;

	QVariantHash data;
	data["id"] = id;
	data["method"] = QByteArray("accept");
	data["args"] = args;

	QByteArray buf = TnetString::fromVariant(data);
	wrapper->proxyAcceptSock->write(QList<QByteArray>() << QByteArray() << buf);
	while(!wrapper->acceptSuccess)
		QTest::qWait(10);

	data.clear();

	QVariantHash hr;
	hr["body"] = QByteArray("hello world\n");

	QVariantHash formats;
	formats["http-response"] = hr;

	data["channel"] = QByteArray("apple");
	data["formats"] = formats;

	buf = TnetString::fromVariant(data);
	wrapper->publishPushSock->write(QList<QByteArray>() << buf);
	while(!wrapper->finished)
		QTest::qWait(10);

	TEST_ASSERT(wrapper->responses.contains(id));
	TEST_ASSERT_EQ(wrapper->responses.value(id).body, QByteArray("hello world\n"));
}

static void publishStream()
{
	TestQCoreApplication qapp;
	TestState state;
	Wrapper *wrapper = state.wrapper;

	wrapper->reset();

	QByteArray id = "6";

	QVariantHash rid;
	rid["sender"] = QByteArray("test-client");
	rid["id"] = id;

	QVariantHash reqState;
	reqState["rid"] = rid;
	reqState["in-seq"] = 1;
	reqState["out-seq"] = 1;
	reqState["out-credits"] = 1000;

	QVariantHash req;
	req["method"] = QByteArray("GET");
	req["uri"] = QByteArray("http://example.com/path");
	QVariantList reqHeaders;
	req["headers"] = reqHeaders;
	req["body"] = QByteArray();

	QVariantHash resp;
	resp["code"] = 200;
	resp["reason"] = QByteArray("OK");
	QVariantList respHeaders;
	respHeaders += QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
	respHeaders += QVariant(QVariantList() << QByteArray("Grip-Hold") << QByteArray("stream"));
	respHeaders += QVariant(QVariantList() << QByteArray("Grip-Channel") << QByteArray("apple"));
	resp["headers"] = respHeaders;
	resp["body"] = QByteArray("stream open\n");

	QVariantHash args;
	args["requests"] = QVariantList() << reqState;
	args["request-data"] = req;
	args["orig-request-data"] = req;
	args["response"] = resp;

	QVariantHash data;
	data["id"] = id;
	data["method"] = QByteArray("accept");
	data["args"] = args;

	QByteArray buf = TnetString::fromVariant(data);
	wrapper->proxyAcceptSock->write(QList<QByteArray>() << QByteArray() << buf);
	while(!wrapper->acceptSuccess)
		QTest::qWait(10);

	data.clear();

	{
		QVariantHash hs;
		hs["content"] = QByteArray("hello world\n");

		QVariantHash formats;
		formats["http-stream"] = hs;

		data["channel"] = QByteArray("apple");
		data["formats"] = formats;
	}

	buf = TnetString::fromVariant(data);
	wrapper->publishPushSock->write(QList<QByteArray>() << buf);

	data.clear();

	{
		QVariantHash hs;
		hs["action"] = QByteArray("close");

		QVariantHash formats;
		formats["http-stream"] = hs;

		data["channel"] = QByteArray("apple");
		data["formats"] = formats;
	}

	buf = TnetString::fromVariant(data);
	wrapper->publishPushSock->write(QList<QByteArray>() << buf);

	while(!wrapper->finished)
		QTest::qWait(10);

	TEST_ASSERT(wrapper->responses.contains(id));
	TEST_ASSERT_EQ(wrapper->responses.value(id).body, QByteArray("stream open\nhello world\n"));
}

static void publishStreamReorder()
{
	TestQCoreApplication qapp;
	TestState state;
	Wrapper *wrapper = state.wrapper;

	wrapper->reset();

	QByteArray id = "7";

	QVariantHash rid;
	rid["sender"] = QByteArray("test-client");
	rid["id"] = id;

	QVariantHash reqState;
	reqState["rid"] = rid;
	reqState["in-seq"] = 1;
	reqState["out-seq"] = 1;
	reqState["out-credits"] = 1000;

	QVariantHash req;
	req["method"] = QByteArray("GET");
	req["uri"] = QByteArray("http://example.com/path");
	QVariantList reqHeaders;
	req["headers"] = reqHeaders;
	req["body"] = QByteArray();

	QVariantHash resp;
	resp["code"] = 200;
	resp["reason"] = QByteArray("OK");
	QVariantList respHeaders;
	respHeaders += QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
	respHeaders += QVariant(QVariantList() << QByteArray("Grip-Hold") << QByteArray("stream"));
	respHeaders += QVariant(QVariantList() << QByteArray("Grip-Channel") << QByteArray("apple"));
	resp["headers"] = respHeaders;
	resp["body"] = QByteArray("stream open\n");

	QVariantHash args;
	args["requests"] = QVariantList() << reqState;
	args["request-data"] = req;
	args["orig-request-data"] = req;
	args["response"] = resp;

	QVariantHash data;
	data["id"] = id;
	data["method"] = QByteArray("accept");
	data["args"] = args;

	QByteArray buf = TnetString::fromVariant(data);
	wrapper->proxyAcceptSock->write(QList<QByteArray>() << QByteArray() << buf);
	while(!wrapper->acceptSuccess)
		QTest::qWait(10);

	data.clear();

	{
		QVariantHash hs;
		hs["content"] = QByteArray("one\n");

		QVariantHash formats;
		formats["http-stream"] = hs;

		data["channel"] = QByteArray("apple");
		data["id"] = QByteArray("a");
		data["formats"] = formats;
	}

	buf = TnetString::fromVariant(data);
	wrapper->publishPushSock->write(QList<QByteArray>() << buf);

	data.clear();

	{
		QVariantHash hs;
		hs["action"] = QByteArray("close");

		QVariantHash formats;
		formats["http-stream"] = hs;

		data["channel"] = QByteArray("apple");
		data["id"] = QByteArray("e");
		data["prev-id"] = QByteArray("d");
		data["formats"] = formats;
	}

	buf = TnetString::fromVariant(data);
	wrapper->publishPushSock->write(QList<QByteArray>() << buf);

	data.clear();

	{
		QVariantHash hs;
		hs["content"] = QByteArray("four\n");

		QVariantHash formats;
		formats["http-stream"] = hs;

		data["channel"] = QByteArray("apple");
		data["id"] = QByteArray("d");
		data["prev-id"] = QByteArray("c");
		data["formats"] = formats;
	}

	buf = TnetString::fromVariant(data);
	wrapper->publishPushSock->write(QList<QByteArray>() << buf);

	data.clear();

	{
		QVariantHash hs;
		hs["content"] = QByteArray("three\n");

		QVariantHash formats;
		formats["http-stream"] = hs;

		data["channel"] = QByteArray("apple");
		data["id"] = QByteArray("c");
		data["prev-id"] = QByteArray("b");
		data["formats"] = formats;
	}

	buf = TnetString::fromVariant(data);
	wrapper->publishPushSock->write(QList<QByteArray>() << buf);

	data.clear();

	{
		QVariantHash hs;
		hs["content"] = QByteArray("two\n");

		QVariantHash formats;
		formats["http-stream"] = hs;

		data["channel"] = QByteArray("apple");
		data["id"] = QByteArray("b");
		data["prev-id"] = QByteArray("a");
		data["formats"] = formats;
	}

	buf = TnetString::fromVariant(data);
	wrapper->publishPushSock->write(QList<QByteArray>() << buf);

	while(!wrapper->finished)
		QTest::qWait(10);

	TEST_ASSERT(wrapper->responses.contains(id));
	TEST_ASSERT_EQ(wrapper->responses.value(id).body, QByteArray("stream open\none\ntwo\nthree\nfour\n"));
}

extern "C" int handlerengine_test(ffi::TestException *out_ex)
{
	TEST_CATCH(acceptNoHold());
	TEST_CATCH(acceptNoHoldResponseSent());
	TEST_CATCH(acceptNoHoldNext());
	TEST_CATCH(acceptNoHoldNextResponseSent());
	TEST_CATCH(publishResponse());
	TEST_CATCH(publishStream());
	TEST_CATCH(publishStreamReorder());

	return 0;
}
