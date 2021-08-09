/*
 * Copyright (C) 2016 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#include <QtTest/QtTest>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "qzmqreqmessage.h"
#include "log.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "packet/httpresponsedata.h"
#include "engine.h"

class Wrapper : public QObject
{
	Q_OBJECT

public:
	QZmq::Socket *zhttpClientOutStreamSock;
	QZmq::Socket *zhttpClientInSock;
	QZmq::Valve *zhttpClientInValve;
	QZmq::Socket *zhttpServerInSock;
	QZmq::Valve *zhttpServerInValve;
	QZmq::Socket *zhttpServerInStreamSock;
	QZmq::Valve *zhttpServerInStreamValve;
	QZmq::Socket *zhttpServerOutSock;
	QZmq::Socket *proxyAcceptSock;
	QZmq::Valve *proxyAcceptValve;
	QZmq::Socket *publishPushSock;

	bool acceptSuccess;
	QVariantHash acceptValue;
	bool finished;
	QHash<QByteArray, HttpResponseData> responses;
	int serverReqs;
	bool serverFailed;
	int serverOutSeq;
	QByteArray requestBody;

	Wrapper(QObject *parent) :
		QObject(parent),
		acceptSuccess(false),
		finished(false),
		serverReqs(0),
		serverFailed(false),
		serverOutSeq(0)
	{
		// http sockets

		zhttpClientOutStreamSock = new QZmq::Socket(QZmq::Socket::Router, this);

		zhttpClientInSock = new QZmq::Socket(QZmq::Socket::Sub, this);
		zhttpClientInValve = new QZmq::Valve(zhttpClientInSock, this);
		connect(zhttpClientInValve, &QZmq::Valve::readyRead, this, &Wrapper::zhttpClientIn_readyRead);

		zhttpServerInSock = new QZmq::Socket(QZmq::Socket::Pull, this);
		zhttpServerInValve = new QZmq::Valve(zhttpServerInSock, this);
		connect(zhttpServerInValve, &QZmq::Valve::readyRead, this, &Wrapper::zhttpServerIn_readyRead);

		zhttpServerInStreamSock = new QZmq::Socket(QZmq::Socket::Router, this);
		zhttpServerInStreamSock->setIdentity("test-server");
		zhttpServerInStreamValve = new QZmq::Valve(zhttpServerInStreamSock, this);
		connect(zhttpServerInStreamValve, &QZmq::Valve::readyRead, this, &Wrapper::zhttpServerInStream_readyRead);

		zhttpServerOutSock = new QZmq::Socket(QZmq::Socket::Pub, this);

		// proxy sockets

		proxyAcceptSock = new QZmq::Socket(QZmq::Socket::Dealer, this);
		proxyAcceptValve = new QZmq::Valve(proxyAcceptSock, this);
		connect(proxyAcceptValve, &QZmq::Valve::readyRead, this, &Wrapper::proxyAccept_readyRead);

		// publish sockets

		publishPushSock = new QZmq::Socket(QZmq::Socket::Push, this);
	}

	void startHttp()
	{
		zhttpClientOutStreamSock->bind("ipc://client-out-stream");
		zhttpClientInSock->bind("ipc://client-in");
		zhttpServerInSock->bind("ipc://server-in");
		zhttpServerInStreamSock->bind("ipc://server-in-stream");
		zhttpServerOutSock->bind("ipc://server-out");

		zhttpClientInSock->subscribe("test-client ");

		zhttpClientInValve->open();
		zhttpServerInValve->open();
		zhttpServerInStreamValve->open();
	}

	void startProxy()
	{
		proxyAcceptSock->bind("ipc://accept");

		proxyAcceptValve->open();
	}

	void startPublish()
	{
		publishPushSock->connectToAddress("ipc://publish-pull");
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

private slots:
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

		QVERIFY(v.type() == QVariant::Hash);
		QVariantHash vresp = v.toHash();

		QVERIFY(vresp.value("success").toBool());

		acceptSuccess = true;

		v = vresp.value("value");
		QVERIFY(v.type() == QVariant::Hash);
		acceptValue = v.toHash();
	}
};

class EngineTest : public QObject
{
	Q_OBJECT

private:
	Engine *engine;
	Wrapper *wrapper;

private slots:
	void initTestCase()
	{
		log_setOutputLevel(LOG_LEVEL_WARNING);
		//log_setOutputLevel(LOG_LEVEL_DEBUG);

		wrapper = new Wrapper(this);
		wrapper->startHttp();
		wrapper->startProxy();

		engine = new Engine(this);

		Engine::Configuration config;
		config.instanceId = "handler";
		config.serverInStreamSpecs = QStringList() << "ipc://client-out-stream";
		config.serverOutSpecs = QStringList() << "ipc://client-in";
		config.clientOutSpecs = QStringList() << "ipc://server-in";
		config.clientOutStreamSpecs = QStringList() << "ipc://server-in-stream";
		config.clientInSpecs = QStringList() << "ipc://server-out";
		config.acceptSpec = "ipc://accept";
		config.pushInSpec = "ipc://publish-pull";
		config.connectionSubscriptionMax = 20;
		config.connectionsMax = 20;
		QVERIFY(engine->start(config));

		wrapper->startPublish();

		QTest::qWait(500);
	}

	void cleanupTestCase()
	{
		delete engine;
		delete wrapper;
	}

	void acceptNoHold()
	{
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

		QVERIFY(!wrapper->acceptValue.value("accepted").toBool());
		QCOMPARE(wrapper->acceptValue["response"].toHash()["body"].toByteArray(), QByteArray("hello world\n"));
	}

	void acceptNoHoldResponseSent()
	{
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

		QVERIFY(!wrapper->acceptValue.value("accepted").toBool());
		QVERIFY(!wrapper->acceptValue.contains("response"));
	}

	void acceptNoHoldNext()
	{
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

		QVERIFY(wrapper->acceptValue.value("accepted").toBool());

		while(!wrapper->finished)
			QTest::qWait(10);

		QVERIFY(wrapper->responses.contains(id));
		QCOMPARE(wrapper->responses.value(id).body, QByteArray("hello world\nthis is what's next\n"));
	}

	void acceptNoHoldNextResponseSent()
	{
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

		QVERIFY(wrapper->acceptValue.value("accepted").toBool());

		while(!wrapper->finished)
			QTest::qWait(10);

		QVERIFY(wrapper->responses.contains(id));
		QCOMPARE(wrapper->responses.value(id).body, QByteArray("this is what's next\n"));
	}

	void publishResponse()
	{
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

		QVERIFY(wrapper->responses.contains(id));
		QCOMPARE(wrapper->responses.value(id).body, QByteArray("hello world\n"));
	}

	void publishStream()
	{
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

		QVERIFY(wrapper->responses.contains(id));
		QCOMPARE(wrapper->responses.value(id).body, QByteArray("stream open\nhello world\n"));
	}

	void publishStreamReorder()
	{
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

		QVERIFY(wrapper->responses.contains(id));
		QCOMPARE(wrapper->responses.value(id).body, QByteArray("stream open\none\ntwo\nthree\nfour\n"));
	}
};

QTEST_MAIN(EngineTest)
#include "enginetest.moc"
