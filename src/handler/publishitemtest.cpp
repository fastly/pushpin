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
#include "publishformat.h"
#include "publishitem.h"

static void parseItem()
{
	QVariantHash meta;
	meta["foo"] = QByteArray("bar");
	meta["bar"] = QByteArray("baz");

	QVariantHash hs;
	hs["content"] = QByteArray("hello world");

	QVariantHash formats;
	formats["http-stream"] = hs;

	QVariantHash data;
	data["channel"] = QByteArray("apple");
	data["id"] = QByteArray("item1");
	data["prev-id"] = QByteArray("item0");
	data["meta"] = meta;
	data["formats"] = formats;

	bool ok;
	PublishItem i = PublishItem::fromVariant(data, QString(), &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.channel, QString("apple"));
	TEST_ASSERT_EQ(i.id, QString("item1"));
	TEST_ASSERT_EQ(i.prevId, QString("item0"));
	TEST_ASSERT_EQ(i.meta.count(), 2);
	TEST_ASSERT_EQ(i.meta.value("foo"), QString("bar"));
	TEST_ASSERT_EQ(i.meta.value("bar"), QString("baz"));
	TEST_ASSERT(i.formats.contains(PublishFormat::HttpStream));
	TEST_ASSERT_EQ(i.formats.value(PublishFormat::HttpStream).body, QByteArray("hello world"));
}

static void parseItemJsonStyle()
{
	QVariantMap meta;
	meta["foo"] = QString("bar");
	meta["bar"] = QString("baz");

	QVariantMap hs;
	hs["content"] = QString("hello world");

	QVariantMap formats;
	formats["http-stream"] = hs;

	QVariantMap data;
	data["channel"] = QString("apple");
	data["id"] = QString("item1");
	data["prev-id"] = QString("item0");
	data["meta"] = meta;
	data["formats"] = formats;

	bool ok;
	PublishItem i = PublishItem::fromVariant(data, QString(), &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(i.channel, QString("apple"));
	TEST_ASSERT_EQ(i.id, QString("item1"));
	TEST_ASSERT_EQ(i.prevId, QString("item0"));
	TEST_ASSERT_EQ(i.meta.count(), 2);
	TEST_ASSERT_EQ(i.meta.value("foo"), QString("bar"));
	TEST_ASSERT_EQ(i.meta.value("bar"), QString("baz"));
	TEST_ASSERT(i.formats.contains(PublishFormat::HttpStream));
	TEST_ASSERT_EQ(i.formats.value(PublishFormat::HttpStream).body, QByteArray("hello world"));
}

extern "C" int publishitem_test(ffi::TestException *out_ex)
{
	TEST_CATCH(parseItem());
	TEST_CATCH(parseItemJsonStyle());

	return 0;
}
