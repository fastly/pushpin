/*
 * Copyright (C) 2013 Fanout, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <QtTest/QtTest>
#include <QtCrypto>
#include <qjson/parser.h>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "qzmqreqmessage.h"
#include "log.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "engine.h"

class Wrapper : public QObject
{
	Q_OBJECT

public:
	QZmq::Socket *zhttpClientOutSock;
	QZmq::Socket *zhttpClientOutStreamSock;
	QZmq::Socket *zhttpClientInSock;
	QZmq::Valve *zhttpClientInValve;
	QZmq::Socket *zhttpServerInSock;
	QZmq::Valve *zhttpServerInValve;
	QZmq::Socket *zhttpServerInStreamSock;
	QZmq::Valve *zhttpServerInStreamValve;
	QZmq::Socket *zhttpServerOutSock;

	QZmq::Socket *handlerInspectSock;
	QZmq::Valve *handlerInspectValve;
	QZmq::Socket *handlerAcceptSock;
	QZmq::Valve *handlerAcceptValve;
	QZmq::Socket *handlerRetryOutSock;

	int serverReqs;
	bool inspectEnabled;
	QByteArray sharingKey;
	QByteArray in;
	QByteArray acceptIn;
	bool retried;
	bool finished;
	int clientReqsFinished;

	Wrapper(QObject *parent) :
		QObject(parent),
		serverReqs(0),
		inspectEnabled(true),
		retried(false),
		finished(false),
		clientReqsFinished(0)
	{
		// http sockets

		zhttpClientOutSock = new QZmq::Socket(QZmq::Socket::Push, this);

		zhttpClientOutStreamSock = new QZmq::Socket(QZmq::Socket::Router, this);

		zhttpClientInSock = new QZmq::Socket(QZmq::Socket::Sub, this);
		zhttpClientInValve = new QZmq::Valve(zhttpClientInSock, this);
		connect(zhttpClientInValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(zhttpClientIn_readyRead(const QList<QByteArray> &)));

		zhttpServerInSock = new QZmq::Socket(QZmq::Socket::Pull, this);
		zhttpServerInValve = new QZmq::Valve(zhttpServerInSock, this);
		connect(zhttpServerInValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(zhttpServerIn_readyRead(const QList<QByteArray> &)));

		zhttpServerInStreamSock = new QZmq::Socket(QZmq::Socket::Router, this);
		zhttpServerInStreamValve = new QZmq::Valve(zhttpServerInStreamSock, this);
		connect(zhttpServerInStreamValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(zhttpServerInStream_readyRead(const QList<QByteArray> &)));

		zhttpServerOutSock = new QZmq::Socket(QZmq::Socket::Pub, this);

		// handler sockets

		handlerInspectSock = new QZmq::Socket(QZmq::Socket::Router, this);

		handlerAcceptSock = new QZmq::Socket(QZmq::Socket::Router, this);
		handlerAcceptValve = new QZmq::Valve(handlerAcceptSock, this);
		connect(handlerAcceptValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(handlerAccept_readyRead(const QList<QByteArray> &)));

		handlerInspectValve = new QZmq::Valve(handlerInspectSock, this);
		connect(handlerInspectValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(handlerInspect_readyRead(const QList<QByteArray> &)));

		handlerRetryOutSock = new QZmq::Socket(QZmq::Socket::Push, this);
	}

	void startHttp()
	{
		zhttpClientOutSock->bind("ipc://client-out");
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

	void startHandler()
	{
		handlerInspectSock->connectToAddress("ipc://inspect");
		handlerAcceptSock->connectToAddress("ipc://accept");
		handlerRetryOutSock->connectToAddress("ipc://retry-out");

		handlerInspectValve->open();
		handlerAcceptValve->open();
	}

	void reset()
	{
		serverReqs = 0;
		inspectEnabled = true;
		sharingKey.clear();
		in.clear();
		acceptIn.clear();
		retried = false;
		finished = false;
		clientReqsFinished = 0;
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
			in += zresp.body;
			if(!zresp.more)
			{
				finished = true;
				++clientReqsFinished;
			}
		}
		else if(zresp.type == ZhttpResponsePacket::HandoffStart)
		{
			ZhttpRequestPacket zreq;
			zreq.from = "test-client";
			zreq.id = zresp.id;
			zreq.type = ZhttpRequestPacket::HandoffProceed;
			zreq.seq = 1;
			QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
			log_debug("writing: %s", buf.data());
			QList<QByteArray> msg;
			msg.append("proxy");
			msg.append(QByteArray());
			msg.append(buf);
			zhttpClientOutStreamSock->write(msg);
		}
	}

	void zhttpServerIn_readyRead(const QList<QByteArray> &message)
	{
		++serverReqs;
		log_debug("server in");
		QVariant v = TnetString::toVariant(message[0].mid(1));
		ZhttpRequestPacket zreq;
		zreq.fromVariant(v);

		ZhttpResponsePacket zresp;
		zresp.from = "test-server";
		zresp.id = zreq.id;
		zresp.seq = 0;
		zresp.code = 200;
		zresp.reason = "OK";
		if(!retried && zreq.uri.encodedQuery().contains("wait=true"))
		{
			if(zreq.uri.encodedPath() == "/path2")
			{
				zresp.body = "{ \"hold\": { \"mode\": \"response\", \"channels\": [ { \"name\": \"test-channel\", \"prev-id\": \"1\" } ] } }";
				zresp.headers += HttpHeader("Content-Type", "application/grip-instruct");
			}
			else
			{
				zresp.body = "{ \"hold\": { \"mode\": \"response\", \"channels\": [ { \"name\": \"test-channel\" } ] } }";
				zresp.headers += HttpHeader("Content-Type", "application/grip-instruct");
			}
		}
		else
		{
			if(zreq.uri.encodedPath().startsWith("/jsonp"))
			{
				zresp.body = "{\"hello\": \"world\"}";
				zresp.headers += HttpHeader("Content-Type", "application/json");
			}
			else
			{
				zresp.body = "hello world";
				zresp.headers += HttpHeader("Content-Type", "text/plain");
			}
		}
		zresp.headers += HttpHeader("Content-Length", QByteArray::number(zresp.body.size()));
		QByteArray buf = zreq.from + " T" + TnetString::fromVariant(zresp.toVariant());
		zhttpServerOutSock->write(QList<QByteArray>() << buf);
	}

	void zhttpServerInStream_readyRead(const QList<QByteArray> &message)
	{
		Q_UNUSED(message);
	}

	void handlerInspect_readyRead(const QList<QByteArray> &_message)
	{
		QZmq::ReqMessage message(_message);
		QVariant v = TnetString::toVariant(message.content()[0]);
		log_debug("inspect: %s", qPrintable(TnetString::variantToString(v, -1)));
		if(inspectEnabled)
		{
			QVariantHash vreq = v.toHash();
			QVariantHash args = vreq["args"].toHash();
			QVariantHash respValue;
			respValue["no-proxy"] = false;
			if(!sharingKey.isEmpty())
				respValue["sharing-key"] = sharingKey;
			QVariantHash vresp;
			vresp["id"] = vreq["id"];
			vresp["success"] = true;
			vresp["value"] = respValue;
			log_debug("inspect response: %s", qPrintable(TnetString::variantToString(vresp, -1)));
			handlerInspectSock->write(message.createReply(QList<QByteArray>() << TnetString::fromVariant(vresp)).toRawMessage());
		}
	}

	void handlerAccept_readyRead(const QList<QByteArray> &_message)
	{
		QZmq::ReqMessage message(_message);
		QVariant v = TnetString::toVariant(message.content()[0]);
		log_debug("inspect: %s", qPrintable(TnetString::variantToString(v, -1)));

		QVariantHash vreq = v.toHash();

		QVariantHash vresp;
		vresp["id"] = vreq["id"];
		vresp["success"] = true;
		QVariantHash respValue;
		respValue["accepted"] = true;
		vresp["value"] = respValue;
		handlerAcceptSock->write(message.createReply(QList<QByteArray>() << TnetString::fromVariant(vresp)).toRawMessage());

		QVariantHash vaccept = vreq["args"].toHash();
		acceptIn = vaccept["response"].toHash()["body"].toByteArray();

		bool ok;
		QJson::Parser parser;
		log_debug("instruct: [%s]", acceptIn.data());
		QVariant vinstruct = parser.parse(acceptIn, &ok);
		QVERIFY(ok && vinstruct.type() == QVariant::Map);
		QVariantMap instruct = vinstruct.toMap();

		if(instruct["hold"].toMap()["channels"].toList()[0].toMap().contains("prev-id"))
		{
			retried = true;
			QVariantHash vretry;
			vretry["requests"] = vaccept["requests"];
			vretry["request-data"] = vaccept["request-data"];
			QByteArray buf = TnetString::fromVariant(vretry);
			log_debug("retrying: %s", buf.data());
			handlerRetryOutSock->write(QList<QByteArray>() << buf);
		}
		else
			finished = true;
	}
};

class EngineTest : public QObject
{
	Q_OBJECT

private:
	QCA::Initializer *qcaInit;
	Engine *engine;
	Wrapper *wrapper;

private slots:
	void initTestCase()
	{
		qcaInit = new QCA::Initializer;
		QVERIFY(QCA::isSupported("hmac(sha256)"));

		log_setOutputLevel(LOG_LEVEL_WARNING);
		//log_setOutputLevel(LOG_LEVEL_DEBUG);

		wrapper = new Wrapper(this);
		wrapper->startHttp();

		engine = new Engine(this);

		Engine::Configuration config;
		config.clientId = "proxy";
		config.serverInSpecs = QStringList() << "ipc://client-out";
		config.serverInStreamSpecs = QStringList() << "ipc://client-out-stream";
		config.serverOutSpecs = QStringList() << "ipc://client-in";
		config.clientOutSpecs = QStringList() << "ipc://server-in";
		config.clientOutStreamSpecs = QStringList() << "ipc://server-in-stream";
		config.clientInSpecs = QStringList() << "ipc://server-out";
		config.inspectSpec = "ipc://inspect";
		config.acceptSpec = "ipc://accept";
		config.retryInSpec = "ipc://retry-out";
		config.inspectTimeout = 500;
		config.routesFile = "routes";
		config.sigIss = "pushpin";
		config.sigKey = "changeme";
		QVERIFY(engine->start(config));

		wrapper->startHandler();

		QTest::qWait(500);
	}

	void cleanupTestCase()
	{
		delete engine;
		delete wrapper;
		delete qcaInit;
	}

	void passthrough()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.id = "1";
		zreq.uri = "http://example/path";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->in, QByteArray("hello world"));
	}

	void passthroughWithoutInspect()
	{
		wrapper->reset();
		wrapper->inspectEnabled = false;

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.id = "2";
		zreq.uri = "http://example/path";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->in, QByteArray("hello world"));
	}

	void passthroughJsonp()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.id = "3";
		zreq.uri = "http://example/jsonp?callback=jpcb";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QVERIFY(wrapper->in.startsWith("/**/jpcb({"));
		QVERIFY(wrapper->in.endsWith("});\n"));
		QByteArray dataRaw = wrapper->in.mid(9, wrapper->in.size() - 9 - 3);

		bool ok;
		QJson::Parser parser;
		QVariant vdata = parser.parse(dataRaw, &ok);
		QVERIFY(ok && vdata.type() == QVariant::Map);
		QVariantMap data = vdata.toMap();

		QCOMPARE(data["code"].toInt(), 200);
		QCOMPARE(data["body"].toByteArray(), QByteArray("{\"hello\": \"world\"}"));
	}

	void passthroughJsonpBasic()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.id = "4";
		zreq.uri = "http://example/jsonp-basic?bparam={}";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->in, QByteArray("/**/jpcb({\"hello\": \"world\"});\n"));
	}

	void accept()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.id = "5";
		zreq.uri = "http://example/path?wait=true";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->acceptIn, QByteArray("{ \"hold\": { \"mode\": \"response\", \"channels\": [ { \"name\": \"test-channel\" } ] } }"));
	}

	void acceptWithRetry()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.id = "6";
		zreq.uri = "http://example/path2?wait=true";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->in, QByteArray("hello world"));
	}

	void passthroughShared()
	{
		wrapper->reset();
		wrapper->sharingKey = "test";

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.uri = "http://example/path";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;

		QByteArray buf;

		// send two requests

		zreq.id = "7";
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);

		zreq.id = "8";
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);

		while(wrapper->clientReqsFinished < 2)
			QTest::qWait(10);

		// there should have only been 1 request to the server
		QCOMPARE(wrapper->serverReqs, 1);

		// hackishly compare the merged inputs
		QCOMPARE(wrapper->in, QByteArray("hello worldhello world"));
	}
};

QTEST_MAIN(EngineTest)
#include "enginetest.moc"
