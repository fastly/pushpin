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
#include "qzmqsocket.h"
#include "qzmqvalve.h"
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
	QZmq::Socket *handlerRetryOutSock;
	QZmq::Socket *handlerAcceptInSock;
	QZmq::Valve *handlerAcceptInValve;

	QByteArray in;
	bool finished;

	Wrapper(QObject *parent) :
		QObject(parent),
		finished(false)
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

		handlerInspectValve = new QZmq::Valve(handlerInspectSock, this);
		connect(handlerInspectValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(handlerInspect_readyRead(const QList<QByteArray> &)));

		handlerRetryOutSock = new QZmq::Socket(QZmq::Socket::Push, this);

		handlerAcceptInSock = new QZmq::Socket(QZmq::Socket::Pull, this);
		handlerAcceptInValve = new QZmq::Valve(handlerAcceptInSock, this);
		connect(handlerAcceptInValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(handlerAcceptIn_readyRead(const QList<QByteArray> &)));
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
		handlerRetryOutSock->connectToAddress("ipc://retry-out");
		handlerAcceptInSock->connectToAddress("ipc://accept-in");

		handlerInspectValve->open();
		handlerAcceptInValve->open();
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
				finished = true;
		}
	}

	void zhttpServerIn_readyRead(const QList<QByteArray> &message)
	{
		log_debug("server in");
		QVariant v = TnetString::toVariant(message[0].mid(1));
		ZhttpRequestPacket zreq;
		zreq.fromVariant(v);

		ZhttpResponsePacket zresp;
		zresp.from = "test-client";
		zresp.id = zreq.id;
		zresp.seq = 0;
		zresp.code = 200;
		zresp.reason = "OK";
		zresp.body = "hello world";
		zresp.headers += HttpHeader("Content-Length", QByteArray::number(zresp.body.size()));
		QByteArray buf = zreq.from + " T" + TnetString::fromVariant(zresp.toVariant());
		zhttpServerOutSock->write(QList<QByteArray>() << buf);
	}

	void zhttpServerInStream_readyRead(const QList<QByteArray> &message)
	{
		Q_UNUSED(message);
	}

	void handlerInspect_readyRead(const QList<QByteArray> &message)
	{
		Q_UNUSED(message);
	}

	void handlerAcceptIn_readyRead(const QList<QByteArray> &message)
	{
		Q_UNUSED(message);
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

		log_setOutputLevel(LOG_LEVEL_INFO);

		wrapper = new Wrapper(this);
		wrapper->startHttp();

		engine = new Engine(this);

		Engine::Configuration config;
		config.clientId = "test";
		config.serverInSpecs = QStringList() << "ipc://client-out";
		config.serverInStreamSpecs = QStringList() << "ipc://client-out-stream";
		config.serverOutSpecs = QStringList() << "ipc://client-in";
		config.clientOutSpecs = QStringList() << "ipc://server-in";
		config.clientOutStreamSpecs = QStringList() << "ipc://server-in-stream";
		config.clientInSpecs = QStringList() << "ipc://server-out";
		config.inspectSpec = "ipc://inspect";
		config.retryInSpec = "ipc://retry-out";
		config.acceptOutSpec = "ipc://accept-in";
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
};

QTEST_MAIN(EngineTest)
#include "enginetest.moc"
