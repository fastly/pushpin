/*
 * Copyright (C) 2016 Fanout, Inc.
 *
 * This file is part of Pushpin.
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
 */

#include <QtTest/QtTest>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "qzmqreqmessage.h"
#include "log.h"
#include "tnetstring.h"
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
	QZmq::Socket *proxyAcceptSock;
	QZmq::Valve *proxyAcceptValve;
	QZmq::Socket *publishPushSock;

	bool accepted;
	bool finished;
	QHash<QByteArray, HttpResponseData> responses;

	Wrapper(QObject *parent) :
		QObject(parent),
		accepted(false),
		finished(false)
	{
		// http sockets

		zhttpClientOutStreamSock = new QZmq::Socket(QZmq::Socket::Router, this);

		zhttpClientInSock = new QZmq::Socket(QZmq::Socket::Sub, this);
		zhttpClientInValve = new QZmq::Valve(zhttpClientInSock, this);
		connect(zhttpClientInValve, &QZmq::Valve::readyRead, this, &Wrapper::zhttpClientIn_readyRead);

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

		zhttpClientInSock->subscribe("test-client ");

		zhttpClientInValve->open();
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
		accepted = false;
		finished = false;
		responses.clear();
	}

private slots:
	void zhttpClientIn_readyRead(const QList<QByteArray> &message)
	{
		int at = message[0].indexOf(' ');
		QVariant v = TnetString::toVariant(message[0].mid(at + 2));
		ZhttpResponsePacket zresp;
		zresp.fromVariant(v);
		if(zresp.type == ZhttpResponsePacket::Data)
		{
			if(!responses.contains(zresp.id))
			{
				HttpResponseData rd;
				rd.code = zresp.code;
				rd.reason = zresp.reason;
				rd.headers = zresp.headers;
				responses[zresp.id] = rd;
			}

			responses[zresp.id].body += zresp.body;

			if(!zresp.more)
				finished = true;
		}
	}

	void proxyAccept_readyRead(const QList<QByteArray> &_message)
	{
		QZmq::ReqMessage message(_message);
		QVariant v = TnetString::toVariant(message.content()[0]);

		QVERIFY(v.type() == QVariant::Hash);
		QVariantHash vresp = v.toHash();

		QVERIFY(vresp.value("success").toBool());

		accepted = true;
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
		config.acceptSpec = "ipc://accept";
		config.pushInSpec = "ipc://publish-pull";
		QVERIFY(engine->start(config));

		wrapper->startPublish();

		QTest::qWait(500);
	}

	void cleanupTestCase()
	{
		delete engine;
		delete wrapper;
	}

	void publish()
	{
		wrapper->reset();

		QVariantHash rid;
		rid["sender"] = QByteArray("test-client");
		rid["id"] = QByteArray("1");

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
		args["response"] = resp;

		QVariantHash data;
		data["id"] = QByteArray("1");
		data["method"] = QByteArray("accept");
		data["args"] = args;

		QByteArray buf = TnetString::fromVariant(data);
		wrapper->proxyAcceptSock->write(QList<QByteArray>() << QByteArray() << buf);
		while(!wrapper->accepted)
			QTest::qWait(10);

		data.clear();

		QVariantHash hr;
		hr["body"] = QByteArray("hello world");

		QVariantHash formats;
		formats["http-response"] = hr;

		data["channel"] = QByteArray("apple");
		data["formats"] = formats;

		buf = TnetString::fromVariant(data);
		wrapper->publishPushSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QVERIFY(wrapper->responses.contains("1"));
		QCOMPARE(wrapper->responses.value("1").body, QByteArray("hello world"));
	}
};

QTEST_MAIN(EngineTest)
#include "enginetest.moc"
