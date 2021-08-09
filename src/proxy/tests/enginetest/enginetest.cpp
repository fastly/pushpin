/*
 * Copyright (C) 2013-2016 Fanout, Inc.
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

#include <unistd.h>
#include <QtTest/QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "qzmqreqmessage.h"
#include "log.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "packet/httpresponsedata.h"
#include "engine.h"

// NOTE: based on proxysession hardcoded max
#define PROXY_MAX_ACCEPT_RESPONSE_BODY 100000

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
	bool serverFailed;
	bool inspectEnabled;
	bool inspected;
	QByteArray sharingKey;
	QByteArray in;
	HttpHeaders acceptHeaders;
	QByteArray acceptIn;
	bool retried;
	bool finished;
	int serverOutSeq;
	int clientReqsFinished;
	QByteArray requestBody;
	QHash<QByteArray, HttpResponseData> responses;

	Wrapper(QObject *parent) :
		QObject(parent),
		serverReqs(0),
		serverFailed(false),
		inspectEnabled(true),
		inspected(false),
		retried(false),
		finished(false),
		serverOutSeq(0),
		clientReqsFinished(0)
	{
		// http sockets

		zhttpClientOutSock = new QZmq::Socket(QZmq::Socket::Push, this);

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

		// handler sockets

		handlerInspectSock = new QZmq::Socket(QZmq::Socket::Router, this);

		handlerAcceptSock = new QZmq::Socket(QZmq::Socket::Router, this);
		handlerAcceptValve = new QZmq::Valve(handlerAcceptSock, this);
		connect(handlerAcceptValve, &QZmq::Valve::readyRead, this, &Wrapper::handlerAccept_readyRead);

		handlerInspectValve = new QZmq::Valve(handlerInspectSock, this);
		connect(handlerInspectValve, &QZmq::Valve::readyRead, this, &Wrapper::handlerInspect_readyRead);

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
		serverFailed = false;
		inspectEnabled = true;
		inspected = false;
		sharingKey.clear();
		in.clear();
		acceptHeaders.clear();
		acceptIn.clear();
		retried = false;
		finished = false;
		serverOutSeq = 0;
		clientReqsFinished = 0;
		requestBody.clear();
		responses.clear();
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
			zreq.ids += ZhttpRequestPacket::Id(zresp.ids.first().id, 1);
			zreq.type = ZhttpRequestPacket::HandoffProceed;
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

		QUrlQuery query(zreq.uri.query());
		QString hold = query.queryItemValue("hold");
		bool bodyInstruct = (query.queryItemValue("body-instruct") == "true");
		bool large = (query.queryItemValue("large") == "true");

		if(!retried && (hold == "response" || hold == "stream"))
		{
			if(encPath == "/path2")
			{
				if(bodyInstruct)
				{
					zresp.headers += HttpHeader("Content-Type", "application/grip-instruct");
					zresp.body = "{ \"hold\": { \"mode\": \"response\", \"channels\": [ { \"name\": \"test-channel\", \"prev-id\": \"1\" } ] } }";
				}
				else
				{
					zresp.headers += HttpHeader("Grip-Hold", "response");
					zresp.headers += HttpHeader("Grip-Channel", "test-channel; prev-id=1");
				}
			}
			else
			{
				if(bodyInstruct)
				{
					zresp.headers += HttpHeader("Content-Type", "application/grip-instruct");
					zresp.body = "{ \"hold\": { \"mode\": \"response\", \"channels\": [ { \"name\": \"test-channel\" } ] } }";
				}
				else
				{
					if(hold == "stream")
					{
						zresp.headers += HttpHeader("Grip-Hold", "stream");
						zresp.headers += HttpHeader("Grip-Channel", "test-channel");
						if(large)
							zresp.body = QByteArray(PROXY_MAX_ACCEPT_RESPONSE_BODY + 10000, 'a') + '\n';
						else
							zresp.body = "stream open\n";
					}
					else
					{
						zresp.headers += HttpHeader("Grip-Hold", "response");
						zresp.headers += HttpHeader("Grip-Channel", "test-channel");
					}
				}
			}
		}
		else
		{
			if(encPath.startsWith("/jsonp"))
			{
				zresp.headers += HttpHeader("Content-Type", "application/json");
				zresp.body = "{\"hello\": \"world\"}";
			}
			else if(encPath == "/path3")
			{
				zresp.headers += HttpHeader("Content-Type", "text/plain");
				zresp.body = "next page";
			}
			else
			{
				if(hold == "none")
				{
					if(bodyInstruct)
					{
						zresp.headers += HttpHeader("Content-Type", "application/grip-instruct");
						zresp.body = "{ \"response\": { \"body\": \"hello world\" } }";
					}
					else
					{
						zresp.headers += HttpHeader("Content-Type", "text/plain");
						if(large)
						{
							// Grip-Link required to trigger accept after
							//   sending large response. note that the link
							//   won't be followed in this test since that's
							//   not a proxy issue
							zresp.headers += HttpHeader("Grip-Link", "</path3>; rel=next");
							zresp.body = QByteArray(PROXY_MAX_ACCEPT_RESPONSE_BODY + 10000, 'a') + '\n';
						}
						else
						{
							zresp.headers += HttpHeader("Grip-Foo", "bar"); // something to trigger accept
							zresp.body = "hello world";
						}
					}
				}
				else
				{
					zresp.headers += HttpHeader("Content-Type", "text/plain");
					zresp.body = "hello world";
				}
			}
		}
		zresp.headers += HttpHeader("Content-Length", QByteArray::number(zresp.body.size()));
		QByteArray buf = zreq.from + " T" + TnetString::fromVariant(zresp.toVariant());
		zhttpServerOutSock->write(QList<QByteArray>() << buf);

		// zero out so we can accept another request
		serverOutSeq = 0;
	}

	void handlerInspect_readyRead(const QList<QByteArray> &_message)
	{
		QZmq::ReqMessage message(_message);
		QVariant v = TnetString::toVariant(message.content()[0]);
		log_debug("inspect: %s", qPrintable(TnetString::variantToString(v, -1)));

		inspected = true;

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
		log_debug("accept: %s", qPrintable(TnetString::variantToString(v, -1)));

		QVariantHash vreq = v.toHash();
		QVariantHash vaccept = vreq["args"].toHash();
		QVariantHash vresponse = vaccept["response"].toHash();

		acceptHeaders.clear();
		foreach(const QVariant &vheader, vresponse["headers"].toList())
		{
			QVariantList h = vheader.toList();
			acceptHeaders += HttpHeader(h[0].toByteArray(), h[1].toByteArray());
		}

		acceptIn = vresponse["body"].toByteArray();

		QVariantMap jsonInstruct;
		QByteArray hold;

		if(acceptHeaders.get("Content-Type") == "application/grip-instruct")
		{
			QJsonParseError e;
			QJsonDocument doc = QJsonDocument::fromJson(acceptIn, &e);
			QVERIFY(e.error == QJsonParseError::NoError);
			QVERIFY(doc.isObject());

			jsonInstruct = doc.object().toVariantMap();

			if(jsonInstruct.contains("hold"))
				hold = jsonInstruct["hold"].toMap().value("mode").toString().toUtf8();
		}
		else
		{
			hold = acceptHeaders.get("Grip-Hold");
		}

		QVariantHash vresp;
		vresp["id"] = vreq["id"];
		vresp["success"] = true;
		QVariantHash respValue;
		if(!hold.isEmpty())
			respValue["accepted"] = true;
		else if(!vaccept.value("response-sent").toBool())
			respValue["response"] = vresponse;
		vresp["value"] = respValue;
		handlerAcceptSock->write(message.createReply(QList<QByteArray>() << TnetString::fromVariant(vresp)).toRawMessage());

		log_debug("instruct: [%s]", acceptIn.data());

		if(acceptHeaders.get("Content-Type") == "application/grip-instruct")
		{
			if(jsonInstruct.contains("hold") && jsonInstruct["hold"].toMap()["channels"].toList()[0].toMap().contains("prev-id"))
			{
				retried = true;
				QVariantHash vretry;
				vretry["requests"] = vaccept["requests"];
				vretry["request-data"] = vaccept["request-data"];
				QByteArray buf = TnetString::fromVariant(vretry);
				log_debug("retrying: %s", qPrintable(TnetString::variantToString(vretry, -1)));
				handlerRetryOutSock->write(QList<QByteArray>() << buf);
				return;
			}
		}

		if(!hold.isEmpty())
			finished = true;
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
		config.inspectPrefetch = 5;
		config.routesFile = "routes";
		config.sigIss = "pushpin";
		config.sigKey = "changeme";
		config.connectionsMax = 20;
		QVERIFY(engine->start(config));

		wrapper->startHandler();

		QTest::qWait(500);
	}

	void cleanupTestCase()
	{
		delete engine;
		delete wrapper;
	}

	void passthrough()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("1", 0);
		zreq.type = ZhttpRequestPacket::Data;
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
		zreq.ids += ZhttpRequestPacket::Id("2", 0);
		zreq.type = ZhttpRequestPacket::Data;
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
		zreq.ids += ZhttpRequestPacket::Id("3", 0);
		zreq.type = ZhttpRequestPacket::Data;
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

		QJsonParseError e;
		QJsonDocument doc = QJsonDocument::fromJson(dataRaw, &e);
		QVERIFY(e.error == QJsonParseError::NoError);
		QVERIFY(doc.isObject());

		QVariantMap data = doc.object().toVariantMap();

		QCOMPARE(data["code"].toInt(), 200);
		QCOMPARE(data["body"].toByteArray(), QByteArray("{\"hello\": \"world\"}"));
	}

	void passthroughJsonpBasic()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("4", 0);
		zreq.type = ZhttpRequestPacket::Data;
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

	void passthroughPostStream()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("5", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path";
		zreq.method = "POST";
		zreq.stream = true;
		zreq.body = "hello"; // enough to hit the prefetch amount
		zreq.more = true;
		zreq.credits = 200000;

		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);

		// ensure the server gets hit without finishing the request
		while(wrapper->serverReqs < 1)
			QTest::qWait(10);

		// now finish the request
		zreq = ZhttpRequestPacket();
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("5", 1);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.body = " world";
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		QList<QByteArray> msg;
		msg.append("proxy");
		msg.append(QByteArray());
		msg.append(buf);
		wrapper->zhttpClientOutStreamSock->write(msg);

		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->requestBody, QByteArray("hello world"));
		QCOMPARE(wrapper->responses["5"].body, QByteArray("hello world"));
	}

	void passthroughPostStreamFail()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("6", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path";
		zreq.method = "POST";
		zreq.stream = true;
		zreq.body = "hello"; // enough to hit the prefetch amount
		zreq.more = true;
		zreq.credits = 200000;

		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);

		// ensure the server gets hit without finishing the request
		while(wrapper->serverReqs < 1)
			QTest::qWait(10);

		// now cancel the request
		zreq = ZhttpRequestPacket();
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("6", 1);
		zreq.type = ZhttpRequestPacket::Cancel;
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		QList<QByteArray> msg;
		msg.append("proxy");
		msg.append(QByteArray());
		msg.append(buf);
		wrapper->zhttpClientOutStreamSock->write(msg);

		// wait for server side to receive error
		while(!wrapper->serverFailed)
			QTest::qWait(10);
	}

	void acceptResponse()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("7", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path?hold=response";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->acceptHeaders.get("Grip-Hold"), QByteArray("response"));
		QCOMPARE(wrapper->acceptHeaders.get("Grip-Channel"), QByteArray("test-channel"));
		QVERIFY(wrapper->acceptIn.isEmpty());
	}

	void acceptStream()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("8", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path?hold=stream";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->acceptHeaders.get("Grip-Hold"), QByteArray("stream"));
		QCOMPARE(wrapper->acceptHeaders.get("Grip-Channel"), QByteArray("test-channel"));
		QCOMPARE(wrapper->acceptIn, QByteArray("stream open\n"));
		QVERIFY(wrapper->in.isEmpty());
	}

	void acceptResponseBodyInstruct()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("9", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path?hold=response&body-instruct=true";
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

	void acceptNoHold()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("10", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path?hold=none";
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

	void acceptNoHoldBodyInstruct()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("11", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path?hold=none&body-instruct=true";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->acceptIn, QByteArray("{ \"response\": { \"body\": \"hello world\" } }"));
	}

	void passthroughThenAcceptStream()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("12", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path?hold=stream&large=true";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->in.size(), 110001);
		QCOMPARE(wrapper->in.mid(wrapper->in.size() - 2), QByteArray("a\n"));
		QCOMPARE(wrapper->acceptHeaders.get("Grip-Hold"), QByteArray("stream"));
		QCOMPARE(wrapper->acceptHeaders.get("Grip-Channel"), QByteArray("test-channel"));
		QVERIFY(wrapper->acceptIn.isEmpty());
	}

	void passthroughThenAcceptNext()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("13", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path?hold=none&large=true";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;
		QByteArray buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);
		while(!wrapper->finished)
			QTest::qWait(10);

		QCOMPARE(wrapper->in.size(), 110001);
		QCOMPARE(wrapper->in.mid(wrapper->in.size() - 2), QByteArray("a\n"));
		QVERIFY(wrapper->acceptIn.isEmpty());
		QCOMPARE(wrapper->acceptHeaders.get("Grip-Link"), QByteArray("</path3>; rel=next"));
	}

	void acceptWithRetry()
	{
		wrapper->reset();

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.ids += ZhttpRequestPacket::Id("14", 0);
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path2?wait=true&body-instruct=true";
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
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path";
		zreq.method = "GET";
		zreq.stream = true;
		zreq.credits = 200000;

		QByteArray buf;

		// send two requests

		QByteArray id1 = "15";
		QByteArray id2 = "16";

		zreq.ids = QList<ZhttpRequestPacket::Id>() << ZhttpRequestPacket::Id(id1, 0);
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);

		zreq.ids = QList<ZhttpRequestPacket::Id>() << ZhttpRequestPacket::Id(id2, 0);
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);

		while(wrapper->clientReqsFinished < 2)
			QTest::qWait(10);

		// there should have only been 1 request to the server
		QCOMPARE(wrapper->serverReqs, 1);

		QCOMPARE(wrapper->responses[id1].body, QByteArray("hello world"));
		QCOMPARE(wrapper->responses[id2].body, QByteArray("hello world"));
	}

	void passthroughSharedPost()
	{
		wrapper->reset();
		wrapper->sharingKey = "test";

		ZhttpRequestPacket zreq;
		zreq.from = "test-client";
		zreq.type = ZhttpRequestPacket::Data;
		zreq.uri = "http://example/path";
		zreq.method = "POST";
		zreq.stream = true;
		zreq.body = "hello"; // enough to hit the prefetch amount
		zreq.more = true;
		zreq.credits = 200000;

		QByteArray buf;

		// send two requests

		QByteArray id1 = "17";
		QByteArray id2 = "18";

		zreq.ids = QList<ZhttpRequestPacket::Id>() << ZhttpRequestPacket::Id(id1, 0);
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);

		zreq.ids = QList<ZhttpRequestPacket::Id>() << ZhttpRequestPacket::Id(id2, 0);
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		wrapper->zhttpClientOutSock->write(QList<QByteArray>() << buf);

		// we've hit prefetch, wait for inspect
		while(!wrapper->inspected)
			QTest::qWait(10);

		// finish the requests

		zreq = ZhttpRequestPacket();
		zreq.from = "test-client";
		zreq.type = ZhttpRequestPacket::Data;
		zreq.body = " world";

		zreq.ids = QList<ZhttpRequestPacket::Id>() << ZhttpRequestPacket::Id(id1, 1);
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		QList<QByteArray> msg;
		msg.append("proxy");
		msg.append(QByteArray());
		msg.append(buf);
		wrapper->zhttpClientOutStreamSock->write(msg);

		zreq.ids = QList<ZhttpRequestPacket::Id>() << ZhttpRequestPacket::Id(id2, 1);
		buf = 'T' + TnetString::fromVariant(zreq.toVariant());
		log_debug("writing: %s", buf.data());
		msg.clear();
		msg.append("proxy");
		msg.append(QByteArray());
		msg.append(buf);
		wrapper->zhttpClientOutStreamSock->write(msg);

		while(wrapper->clientReqsFinished < 2)
			QTest::qWait(10);

		// there should have only been 1 request to the server
		QCOMPARE(wrapper->serverReqs, 1);

		QCOMPARE(wrapper->responses[id1].body, QByteArray("hello world"));
		QCOMPARE(wrapper->responses[id2].body, QByteArray("hello world"));
	}
};

QTEST_MAIN(EngineTest)
#include "enginetest.moc"
