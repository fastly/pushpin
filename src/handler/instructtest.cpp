/*
 * Copyright (C) 2016-2019 Fanout, Inc.
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

#include "test.h"
#include "httpheaders.h"
#include "packet/httpresponsedata.h"
#include "instruct.h"

static void noHold()
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
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::NoHold);
	TEST_ASSERT_EQ(i.response.code, 200);
	TEST_ASSERT_EQ(i.response.reason, QByteArray("OK"));
	TEST_ASSERT_EQ(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
	TEST_ASSERT(!i.response.headers.contains("Grip-Channel"));
	TEST_ASSERT_EQ(i.response.body, QByteArray("hello world"));

	data.headers += HttpHeader("Grip-Status", "404");
	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::NoHold);
	TEST_ASSERT_EQ(i.response.code, 404);
	TEST_ASSERT_EQ(i.response.reason, QByteArray("Not Found"));

	data.headers.removeAll("Grip-Status");
	data.headers += HttpHeader("Grip-Status", "404 Nothing To See Here");
	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::NoHold);
	TEST_ASSERT_EQ(i.response.code, 404);
	TEST_ASSERT_EQ(i.response.reason, QByteArray("Nothing To See Here"));

	data.headers.clear();
	data.headers += HttpHeader("Content-Type", "application/grip-instruct");
	data.body = "{\"response\":{\"code\": 200,\"headers\":{\"Content-Type\":\"text/plain\"},\"body\":\"hello world\"}}";

	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::NoHold);
	TEST_ASSERT_EQ(i.response.code, 200);
	TEST_ASSERT_EQ(i.response.reason, QByteArray("OK"));
	TEST_ASSERT_EQ(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
	TEST_ASSERT_EQ(i.response.body, QByteArray("hello world"));
}

static void responseHold()
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
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::ResponseHold);
	TEST_ASSERT_EQ(i.channels.count(), 3);
	TEST_ASSERT_EQ(i.channels[0].name, QString("apple"));
	TEST_ASSERT_EQ(i.channels[1].name, QString("banana"));
	TEST_ASSERT_EQ(i.channels[2].name, QString("cherry"));
	TEST_ASSERT_EQ(i.timeout, 120);
	TEST_ASSERT_EQ(i.meta.count(), 2);
	TEST_ASSERT_EQ(i.meta.value("foo"), QString("bar"));
	TEST_ASSERT_EQ(i.meta.value("bar"), QString("baz"));
	TEST_ASSERT_EQ(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
	TEST_ASSERT(!i.response.headers.contains("Grip-Channel"));
	TEST_ASSERT_EQ(i.response.body, QByteArray("hello world"));

	data.headers.clear();
	data.headers += HttpHeader("Content-Type", "application/grip-instruct");
	data.body = "{\"hold\":{\"mode\":\"response\",\"channels\":[{\"name\":\"test\"}],\"timeout\":120,\"meta\":{\"foo\":\"bar\",\"bar\":\"baz\"}},\"response\":{\"code\": 200,\"headers\":{\"Content-Type\":\"text/plain\"},\"body\":\"hello world\"}}";

	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::ResponseHold);
	TEST_ASSERT_EQ(i.channels.count(), 1);
	TEST_ASSERT_EQ(i.channels[0].name, QString("test"));
	TEST_ASSERT_EQ(i.timeout, 120);
	TEST_ASSERT_EQ(i.meta.count(), 2);
	TEST_ASSERT_EQ(i.meta.value("foo"), QString("bar"));
	TEST_ASSERT_EQ(i.meta.value("bar"), QString("baz"));
	TEST_ASSERT_EQ(i.response.code, 200);
	TEST_ASSERT_EQ(i.response.reason, QByteArray("OK"));
	TEST_ASSERT_EQ(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
	TEST_ASSERT_EQ(i.response.body, QByteArray("hello world"));
}

static void responseHoldChannelParams()
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
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::ResponseHold);
	TEST_ASSERT_EQ(i.channels.count(), 3);
	TEST_ASSERT_EQ(i.channels[0].name, QString("apple"));
	TEST_ASSERT_EQ(i.channels[0].prevId, QString("item1"));
	TEST_ASSERT_EQ(i.channels[0].filters.count(), 1);
	TEST_ASSERT_EQ(i.channels[0].filters[0], QString("f1"));
	TEST_ASSERT_EQ(i.channels[1].name, QString("banana"));
	TEST_ASSERT(i.channels[1].prevId.isNull());
	TEST_ASSERT_EQ(i.channels[1].filters.count(), 1);
	TEST_ASSERT_EQ(i.channels[1].filters[0], QString("f2"));
	TEST_ASSERT_EQ(i.channels[2].name, QString("cherry"));
	TEST_ASSERT(i.channels[2].prevId.isNull());
	TEST_ASSERT_EQ(i.channels[2].filters.count(), 2);
	TEST_ASSERT_EQ(i.channels[2].filters[0], QString("f1"));
	TEST_ASSERT_EQ(i.channels[2].filters[1], QString("f2"));
	TEST_ASSERT_EQ(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
	TEST_ASSERT(!i.response.headers.contains("Grip-Channel"));
	TEST_ASSERT_EQ(i.response.body, QByteArray("hello world"));

	data.headers.clear();
	data.headers += HttpHeader("Content-Type", "application/grip-instruct");
	data.body = "{\"hold\":{\"mode\":\"response\",\"channels\":[{\"name\":\"apple\",\"prev-id\":\"item1\",\"filters\":[\"f1\"]},{\"name\":\"banana\",\"filters\":[\"f2\"]},{\"name\":\"cherry\",\"filters\":[\"f1\",\"f2\"]}]},\"response\":{\"code\": 200,\"headers\":{\"Content-Type\":\"text/plain\"},\"body\":\"hello world\"}}";

	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::ResponseHold);
	TEST_ASSERT_EQ(i.channels.count(), 3);
	TEST_ASSERT_EQ(i.channels[0].name, QString("apple"));
	TEST_ASSERT_EQ(i.channels[0].prevId, QString("item1"));
	TEST_ASSERT_EQ(i.channels[0].filters.count(), 1);
	TEST_ASSERT_EQ(i.channels[0].filters[0], QString("f1"));
	TEST_ASSERT_EQ(i.channels[1].name, QString("banana"));
	TEST_ASSERT(i.channels[1].prevId.isNull());
	TEST_ASSERT_EQ(i.channels[1].filters.count(), 1);
	TEST_ASSERT_EQ(i.channels[1].filters[0], QString("f2"));
	TEST_ASSERT_EQ(i.channels[2].name, QString("cherry"));
	TEST_ASSERT(i.channels[2].prevId.isNull());
	TEST_ASSERT_EQ(i.channels[2].filters.count(), 2);
	TEST_ASSERT_EQ(i.channels[2].filters[0], QString("f1"));
	TEST_ASSERT_EQ(i.channels[2].filters[1], QString("f2"));
	TEST_ASSERT_EQ(i.response.code, 200);
	TEST_ASSERT_EQ(i.response.reason, QByteArray("OK"));
	TEST_ASSERT_EQ(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
	TEST_ASSERT_EQ(i.response.body, QByteArray("hello world"));
}

static void streamHold()
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
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::StreamHold);
	TEST_ASSERT_EQ(i.channels.count(), 3);
	TEST_ASSERT_EQ(i.channels[0].name, QString("apple"));
	TEST_ASSERT_EQ(i.channels[1].name, QString("banana"));
	TEST_ASSERT_EQ(i.channels[2].name, QString("cherry"));
	TEST_ASSERT_EQ(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
	TEST_ASSERT(!i.response.headers.contains("Grip-Channel"));
	TEST_ASSERT_EQ(i.response.body, QByteArray("hello world"));

	data.headers.clear();
	data.headers += HttpHeader("Content-Type", "application/grip-instruct");
	data.body = "{\"hold\":{\"mode\":\"stream\",\"channels\":[{\"name\":\"test\"}]},\"response\":{\"code\": 200,\"headers\":{\"Content-Type\":\"text/plain\"},\"body\":\"hello world\"}}";

	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::StreamHold);
	TEST_ASSERT_EQ(i.channels.count(), 1);
	TEST_ASSERT_EQ(i.channels[0].name, QString("test"));
	TEST_ASSERT_EQ(i.response.code, 200);
	TEST_ASSERT_EQ(i.response.reason, QByteArray("OK"));
	TEST_ASSERT_EQ(i.response.headers.get("Content-Type"), QByteArray("text/plain"));
	TEST_ASSERT_EQ(i.response.body, QByteArray("hello world"));
}

static void streamHoldKeepAlive()
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
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::StreamHold);
	TEST_ASSERT_EQ(i.keepAliveMode, Instruct::NoKeepAlive);

	data.headers.clear();
	data.headers += HttpHeader("Content-Type", "text/plain");
	data.headers += HttpHeader("Grip-Hold", "stream");
	data.headers += HttpHeader("Grip-Channel", "test");
	data.headers += HttpHeader("Grip-Keep-Alive", "ping1\\n; timeout=10");

	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::StreamHold);
	TEST_ASSERT_EQ(i.keepAliveMode, Instruct::Idle);
	TEST_ASSERT_EQ(i.keepAliveData, QByteArray("ping1\\n"));
	TEST_ASSERT_EQ(i.keepAliveTimeout, 10);

	data.headers.clear();
	data.headers += HttpHeader("Content-Type", "text/plain");
	data.headers += HttpHeader("Grip-Hold", "stream");
	data.headers += HttpHeader("Grip-Channel", "test");
	data.headers += HttpHeader("Grip-Keep-Alive", "ping2\\n; format=cstring");

	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::StreamHold);
	TEST_ASSERT_EQ(i.keepAliveMode, Instruct::Idle);
	TEST_ASSERT_EQ(i.keepAliveData, QByteArray("ping2\n"));
	TEST_ASSERT(i.keepAliveTimeout > 0);

	data.headers.clear();
	data.headers += HttpHeader("Content-Type", "text/plain");
	data.headers += HttpHeader("Grip-Hold", "stream");
	data.headers += HttpHeader("Grip-Channel", "test");
	data.headers += HttpHeader("Grip-Keep-Alive", "cGluZzMK; format=base64");

	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::StreamHold);
	TEST_ASSERT_EQ(i.keepAliveMode, Instruct::Idle);
	TEST_ASSERT_EQ(i.keepAliveData, QByteArray("ping3\n"));
	TEST_ASSERT(i.keepAliveTimeout > 0);

	data.headers.clear();
	data.headers += HttpHeader("Content-Type", "text/plain");
	data.headers += HttpHeader("Grip-Hold", "stream");
	data.headers += HttpHeader("Grip-Channel", "test");
	data.headers += HttpHeader("Grip-Keep-Alive", "ping4\\n; mode=interval");

	i = Instruct::fromResponse(data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.holdMode, Instruct::StreamHold);
	TEST_ASSERT_EQ(i.keepAliveMode, Instruct::Interval);
	TEST_ASSERT_EQ(i.keepAliveData, QByteArray("ping4\\n"));
	TEST_ASSERT(i.keepAliveTimeout > 0);
}

extern "C" int instruct_test(ffi::TestException *out_ex)
{
	TEST_CATCH(noHold());
	TEST_CATCH(responseHold());
	TEST_CATCH(responseHoldChannelParams());
	TEST_CATCH(streamHold());
	TEST_CATCH(streamHoldKeepAlive());

	return 0;
}
