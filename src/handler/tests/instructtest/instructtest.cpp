/*
 * Copyright (C) 2016-2019 Fanout, Inc.
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
#include "httpheaders.h"
#include "packet/httpresponsedata.h"
#include "instruct.h"

class InstructTest : public QObject
{
	Q_OBJECT

private slots:
	void noHold()
	{
		HttpResponseData data;
		data.code = 200;
		data.reason = "OK";
		data.headers += HttpHeader("Content-Type", "text/plain");
		data.headers += HttpHeader("Grip-Channel", "test");
		data.body = "hello world";

		Instruct i;
		bool ok;
		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::NoHold);
		QCOMPARE(i.response.code, 200);
		QCOMPARE(i.response.reason, QByteArray("OK"));
		QCOMPARE(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
		QVERIFY(!i.response.headers.contains("Grip-Channel"));
		QCOMPARE(i.response.body, QByteArray("hello world"));

		data.headers += HttpHeader("Grip-Status", "404");
		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::NoHold);
		QCOMPARE(i.response.code, 404);
		QCOMPARE(i.response.reason, QByteArray("Not Found"));

		data.headers.removeAll("Grip-Status");
		data.headers += HttpHeader("Grip-Status", "404 Nothing To See Here");
		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::NoHold);
		QCOMPARE(i.response.code, 404);
		QCOMPARE(i.response.reason, QByteArray("Nothing To See Here"));

		data.headers.clear();
		data.headers += HttpHeader("Content-Type", "application/grip-instruct");
		data.body = "{\"response\":{\"code\": 200,\"headers\":{\"Content-Type\":\"text/plain\"},\"body\":\"hello world\"}}";

		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::NoHold);
		QCOMPARE(i.response.code, 200);
		QCOMPARE(i.response.reason, QByteArray("OK"));
		QCOMPARE(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
		QCOMPARE(i.response.body, QByteArray("hello world"));
	}

	void responseHold()
	{
		HttpResponseData data;
		data.code = 200;
		data.reason = "OK";
		data.headers += HttpHeader("Content-Type", "text/plain");
		data.headers += HttpHeader("Grip-Hold", "response");
		data.headers += HttpHeader("Grip-Channel", "apple");
		data.headers += HttpHeader("Grip-Channel", "banana, cherry");
		data.headers += HttpHeader("Grip-Timeout", "120");
		data.headers += HttpHeader("Grip-Set-Meta", "foo=bar, bar=baz");
		data.body = "hello world";

		Instruct i;
		bool ok;
		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::ResponseHold);
		QCOMPARE(i.channels.count(), 3);
		QCOMPARE(i.channels[0].name, QString("apple"));
		QCOMPARE(i.channels[1].name, QString("banana"));
		QCOMPARE(i.channels[2].name, QString("cherry"));
		QCOMPARE(i.timeout, 120);
		QCOMPARE(i.meta.count(), 2);
		QCOMPARE(i.meta.value("foo"), QString("bar"));
		QCOMPARE(i.meta.value("bar"), QString("baz"));
		QCOMPARE(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
		QVERIFY(!i.response.headers.contains("Grip-Channel"));
		QCOMPARE(i.response.body, QByteArray("hello world"));

		data.headers.clear();
		data.headers += HttpHeader("Content-Type", "application/grip-instruct");
		data.body = "{\"hold\":{\"mode\":\"response\",\"channels\":[{\"name\":\"test\"}],\"timeout\":120,\"meta\":{\"foo\":\"bar\",\"bar\":\"baz\"}},\"response\":{\"code\": 200,\"headers\":{\"Content-Type\":\"text/plain\"},\"body\":\"hello world\"}}";

		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::ResponseHold);
		QCOMPARE(i.channels.count(), 1);
		QCOMPARE(i.channels[0].name, QString("test"));
		QCOMPARE(i.timeout, 120);
		QCOMPARE(i.meta.count(), 2);
		QCOMPARE(i.meta.value("foo"), QString("bar"));
		QCOMPARE(i.meta.value("bar"), QString("baz"));
		QCOMPARE(i.response.code, 200);
		QCOMPARE(i.response.reason, QByteArray("OK"));
		QCOMPARE(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
		QCOMPARE(i.response.body, QByteArray("hello world"));
	}

	void responseHoldChannelParams()
	{
		HttpResponseData data;
		data.code = 200;
		data.reason = "OK";
		data.headers += HttpHeader("Content-Type", "text/plain");
		data.headers += HttpHeader("Grip-Hold", "response");
		data.headers += HttpHeader("Grip-Channel", "apple; prev-id=item1; filter=f1");
		data.headers += HttpHeader("Grip-Channel", "banana; filter=f2, cherry; filter=f1; filter=f2");
		data.body = "hello world";

		Instruct i;
		bool ok;
		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::ResponseHold);
		QCOMPARE(i.channels.count(), 3);
		QCOMPARE(i.channels[0].name, QString("apple"));
		QCOMPARE(i.channels[0].prevId, QString("item1"));
		QCOMPARE(i.channels[0].filters.count(), 1);
		QCOMPARE(i.channels[0].filters[0], QString("f1"));
		QCOMPARE(i.channels[1].name, QString("banana"));
		QVERIFY(i.channels[1].prevId.isNull());
		QCOMPARE(i.channels[1].filters.count(), 1);
		QCOMPARE(i.channels[1].filters[0], QString("f2"));
		QCOMPARE(i.channels[2].name, QString("cherry"));
		QVERIFY(i.channels[2].prevId.isNull());
		QCOMPARE(i.channels[2].filters.count(), 2);
		QCOMPARE(i.channels[2].filters[0], QString("f1"));
		QCOMPARE(i.channels[2].filters[1], QString("f2"));
		QCOMPARE(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
		QVERIFY(!i.response.headers.contains("Grip-Channel"));
		QCOMPARE(i.response.body, QByteArray("hello world"));

		data.headers.clear();
		data.headers += HttpHeader("Content-Type", "application/grip-instruct");
		data.body = "{\"hold\":{\"mode\":\"response\",\"channels\":[{\"name\":\"apple\",\"prev-id\":\"item1\",\"filters\":[\"f1\"]},{\"name\":\"banana\",\"filters\":[\"f2\"]},{\"name\":\"cherry\",\"filters\":[\"f1\",\"f2\"]}]},\"response\":{\"code\": 200,\"headers\":{\"Content-Type\":\"text/plain\"},\"body\":\"hello world\"}}";

		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::ResponseHold);
		QCOMPARE(i.channels.count(), 3);
		QCOMPARE(i.channels[0].name, QString("apple"));
		QCOMPARE(i.channels[0].prevId, QString("item1"));
		QCOMPARE(i.channels[0].filters.count(), 1);
		QCOMPARE(i.channels[0].filters[0], QString("f1"));
		QCOMPARE(i.channels[1].name, QString("banana"));
		QVERIFY(i.channels[1].prevId.isNull());
		QCOMPARE(i.channels[1].filters.count(), 1);
		QCOMPARE(i.channels[1].filters[0], QString("f2"));
		QCOMPARE(i.channels[2].name, QString("cherry"));
		QVERIFY(i.channels[2].prevId.isNull());
		QCOMPARE(i.channels[2].filters.count(), 2);
		QCOMPARE(i.channels[2].filters[0], QString("f1"));
		QCOMPARE(i.channels[2].filters[1], QString("f2"));
		QCOMPARE(i.response.code, 200);
		QCOMPARE(i.response.reason, QByteArray("OK"));
		QCOMPARE(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
		QCOMPARE(i.response.body, QByteArray("hello world"));
	}

	void streamHold()
	{
		HttpResponseData data;
		data.code = 200;
		data.reason = "OK";
		data.headers += HttpHeader("Content-Type", "text/plain");
		data.headers += HttpHeader("Grip-Hold", "stream");
		data.headers += HttpHeader("Grip-Channel", "apple");
		data.headers += HttpHeader("Grip-Channel", "banana, cherry");
		data.body = "hello world";

		Instruct i;
		bool ok;
		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::StreamHold);
		QCOMPARE(i.channels.count(), 3);
		QCOMPARE(i.channels[0].name, QString("apple"));
		QCOMPARE(i.channels[1].name, QString("banana"));
		QCOMPARE(i.channels[2].name, QString("cherry"));
		QCOMPARE(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
		QVERIFY(!i.response.headers.contains("Grip-Channel"));
		QCOMPARE(i.response.body, QByteArray("hello world"));

		data.headers.clear();
		data.headers += HttpHeader("Content-Type", "application/grip-instruct");
		data.body = "{\"hold\":{\"mode\":\"stream\",\"channels\":[{\"name\":\"test\"}]},\"response\":{\"code\": 200,\"headers\":{\"Content-Type\":\"text/plain\"},\"body\":\"hello world\"}}";

		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::StreamHold);
		QCOMPARE(i.channels.count(), 1);
		QCOMPARE(i.channels[0].name, QString("test"));
		QCOMPARE(i.response.code, 200);
		QCOMPARE(i.response.reason, QByteArray("OK"));
		QCOMPARE(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
		QCOMPARE(i.response.body, QByteArray("hello world"));
	}

	void streamHoldKeepAlive()
	{
		HttpResponseData data;
		data.code = 200;
		data.reason = "OK";
		data.body = "hello world";

		data.headers += HttpHeader("Content-Type", "text/plain");
		data.headers += HttpHeader("Grip-Hold", "stream");
		data.headers += HttpHeader("Grip-Channel", "test");
		Instruct i;
		bool ok;
		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::StreamHold);
		QCOMPARE(i.keepAliveMode, Instruct::NoKeepAlive);

		data.headers.clear();
		data.headers += HttpHeader("Content-Type", "text/plain");
		data.headers += HttpHeader("Grip-Hold", "stream");
		data.headers += HttpHeader("Grip-Channel", "test");
		data.headers += HttpHeader("Grip-Keep-Alive", "ping1\\n; timeout=10");

		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::StreamHold);
		QCOMPARE(i.keepAliveMode, Instruct::Idle);
		QCOMPARE(i.keepAliveData, QByteArray("ping1\\n"));
		QCOMPARE(i.keepAliveTimeout, 10);

		data.headers.clear();
		data.headers += HttpHeader("Content-Type", "text/plain");
		data.headers += HttpHeader("Grip-Hold", "stream");
		data.headers += HttpHeader("Grip-Channel", "test");
		data.headers += HttpHeader("Grip-Keep-Alive", "ping2\\n; format=cstring");

		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::StreamHold);
		QCOMPARE(i.keepAliveMode, Instruct::Idle);
		QCOMPARE(i.keepAliveData, QByteArray("ping2\n"));
		QVERIFY(i.keepAliveTimeout > 0);

		data.headers.clear();
		data.headers += HttpHeader("Content-Type", "text/plain");
		data.headers += HttpHeader("Grip-Hold", "stream");
		data.headers += HttpHeader("Grip-Channel", "test");
		data.headers += HttpHeader("Grip-Keep-Alive", "cGluZzMK; format=base64");

		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::StreamHold);
		QCOMPARE(i.keepAliveMode, Instruct::Idle);
		QCOMPARE(i.keepAliveData, QByteArray("ping3\n"));
		QVERIFY(i.keepAliveTimeout > 0);

		data.headers.clear();
		data.headers += HttpHeader("Content-Type", "text/plain");
		data.headers += HttpHeader("Grip-Hold", "stream");
		data.headers += HttpHeader("Grip-Channel", "test");
		data.headers += HttpHeader("Grip-Keep-Alive", "ping4\\n; mode=interval");

		i = Instruct::fromResponse(data, &ok);
		QVERIFY(ok);
		QCOMPARE(i.holdMode, Instruct::StreamHold);
		QCOMPARE(i.keepAliveMode, Instruct::Interval);
		QCOMPARE(i.keepAliveData, QByteArray("ping4\\n"));
		QVERIFY(i.keepAliveTimeout > 0);
	}
};

QTEST_MAIN(InstructTest)
#include "instructtest.moc"
