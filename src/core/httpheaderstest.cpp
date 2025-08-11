/*
 * Copyright (C) 2017 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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
 *
 */

#include "test.h"
#include "httpheaders.h"

static void parseParameters()
{
	HttpHeaders h;
	h += HttpHeader("Fruit", "apple");
	h += HttpHeader("Fruit", "banana");
	h += HttpHeader("Fruit", "cherry");

	QList<HttpHeaderParameters> params = h.getAllAsParameters("Fruit");
	TEST_ASSERT_EQ(params.count(), 3);
	TEST_ASSERT_EQ(params[0][0].first, QByteArray("apple"));
	TEST_ASSERT_EQ(params[1][0].first, QByteArray("banana"));
	TEST_ASSERT_EQ(params[2][0].first, QByteArray("cherry"));

	h.clear();
	h += HttpHeader("Fruit", "apple, banana, cherry");

	params = h.getAllAsParameters("Fruit");
	TEST_ASSERT_EQ(params.count(), 3);
	TEST_ASSERT_EQ(params[0][0].first, QByteArray("apple"));
	TEST_ASSERT_EQ(params[1][0].first, QByteArray("banana"));
	TEST_ASSERT_EQ(params[2][0].first, QByteArray("cherry"));

	h.clear();
	h += HttpHeader("Fruit", "apple; type=\"granny, smith\", banana; type=\"\\\"yellow\\\"\"");

	params = h.getAllAsParameters("Fruit");
	TEST_ASSERT_EQ(params.count(), 2);
	TEST_ASSERT_EQ(params[0][0].first, QByteArray("apple"));
	TEST_ASSERT_EQ(params[0][1].first, QByteArray("type"));
	TEST_ASSERT_EQ(params[0][1].second, QByteArray("granny, smith"));
	TEST_ASSERT_EQ(params[1][0].first, QByteArray("banana"));
	TEST_ASSERT_EQ(params[1][1].first, QByteArray("type"));
	TEST_ASSERT_EQ(params[1][1].second, QByteArray("\"yellow\""));

	h.clear();
	h += HttpHeader("Fruit", "\"apple");

	QList<QByteArray> l = h.getAll("Fruit");
	TEST_ASSERT_EQ(l.count(), 1);
	TEST_ASSERT_EQ(l[0], QByteArray("\"apple"));

	h.clear();
	h += HttpHeader("Fruit", "\"apple\\");

	l = h.getAll("Fruit");
	TEST_ASSERT_EQ(l.count(), 1);
	TEST_ASSERT_EQ(l[0], QByteArray("\"apple\\"));

	h.clear();
	h += HttpHeader("Fruit", "apple; type=gala, banana; type=\"yellow, cherry");

	params = h.getAllAsParameters("Fruit");
	TEST_ASSERT_EQ(params.count(), 1);
	TEST_ASSERT_EQ(params[0][0].first, QByteArray("apple"));
	TEST_ASSERT_EQ(params[0][1].first, QByteArray("type"));
	TEST_ASSERT_EQ(params[0][1].second, QByteArray("gala"));
}

extern "C" int httpheaders_test(ffi::TestException *out_ex)
{
	TEST_CATCH(parseParameters());

	return 0;
}
