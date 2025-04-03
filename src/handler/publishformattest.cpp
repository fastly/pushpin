/*
 * Copyright (C) 2016 Fanout, Inc.
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
#include <QVariant>
#include "publishformat.h"

static void responseFormat()
{
	QVariantHash data;
	data["code"] = 200;
	data["reason"] = QByteArray("OK");
	data["headers"] = QVariantList() << QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
	data["body"] = QByteArray("hello world");

	bool ok;
	PublishFormat f = PublishFormat::fromVariant(PublishFormat::HttpResponse, data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(f.code, 200);
	TEST_ASSERT_EQ(f.reason, QByteArray("OK"));
	TEST_ASSERT_EQ(f.headers.count(), 1);
	TEST_ASSERT_EQ(f.headers[0].first, QByteArray("Content-Type"));
	TEST_ASSERT_EQ(f.headers[0].second, QByteArray("text/plain"));
	TEST_ASSERT_EQ(f.body, QByteArray("hello world"));

	data.clear();
	data["body"] = QByteArray("other fields implied");

	f = PublishFormat::fromVariant(PublishFormat::HttpResponse, data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(f.code, 200);
	TEST_ASSERT_EQ(f.reason, QByteArray("OK"));
	TEST_ASSERT_EQ(f.headers.count(), 0);
	TEST_ASSERT_EQ(f.body, QByteArray("other fields implied"));
}

static void streamFormat()
{
	QVariantHash data;
	data["content"] = QByteArray("hello world");

	bool ok;
	PublishFormat f = PublishFormat::fromVariant(PublishFormat::HttpStream, data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT(f.action == PublishFormat::Send);
	TEST_ASSERT_EQ(f.body, QByteArray("hello world"));

	data.clear();
	data["action"] = QByteArray("close");

	f = PublishFormat::fromVariant(PublishFormat::HttpStream, data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT(f.action == PublishFormat::Close);
	TEST_ASSERT(f.body.isEmpty());
}

static void webSocketMessageFormat()
{
	QVariantHash data;
	data["content"] = QByteArray("hello world");

	bool ok;
	PublishFormat f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT(f.action == PublishFormat::Send);
	TEST_ASSERT_EQ(f.messageType, PublishFormat::Text);
	TEST_ASSERT_EQ(f.body, QByteArray("hello world"));

	data.clear();
	data["type"] = "binary";
	data["content"] = QByteArray("hello world");

	f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT(f.action == PublishFormat::Send);
	TEST_ASSERT_EQ(f.messageType, PublishFormat::Binary);
	TEST_ASSERT_EQ(f.body, QByteArray("hello world"));

	data.clear();
	data["content-bin"] = QByteArray("hello world");

	f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT(f.action == PublishFormat::Send);
	TEST_ASSERT_EQ(f.messageType, PublishFormat::Binary);
	TEST_ASSERT_EQ(f.body, QByteArray("hello world"));

	data.clear();
	data["action"] = "close";

	f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT(f.action == PublishFormat::Close);
	TEST_ASSERT_EQ(f.code, -1);

	data.clear();
	data["action"] = "close";
	data["code"] = 1001;

	f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT(f.action == PublishFormat::Close);
	TEST_ASSERT_EQ(f.code, 1001);
}

extern "C" int publishformat_test(ffi::TestException *out_ex)
{
	TEST_CATCH(responseFormat());
	TEST_CATCH(streamFormat());
	TEST_CATCH(webSocketMessageFormat());

	return 0;
}
