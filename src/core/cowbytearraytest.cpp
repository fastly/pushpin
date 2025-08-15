/*
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
#include "cowbytearray.h"

static void basicUsage()
{
	CowByteArray a("hello");

	// the data is null-terminated, but the null is not counted in the size
	TEST_ASSERT_EQ(a.size(), 5);
	TEST_ASSERT_EQ(strcmp(a.data(), "hello"), 0);

	CowByteArrayList l;
	l += a;
	l += QByteArray("world");
	TEST_ASSERT_EQ(l.count(), 2);
	TEST_ASSERT_EQ(l[0].size(), 5);
	TEST_ASSERT_EQ(l[0].asQByteArray(), "hello");
	TEST_ASSERT_EQ(l[1].asQByteArray(), "world");

	QList<QByteArray> ql;
	ql += "hello";
	ql += "world";
	TEST_ASSERT_EQ(l.asQByteArrayList(), ql);
}

extern "C" int cowbytearray_test(ffi::TestException *out_ex)
{
	TEST_CATCH(basicUsage());

	return 0;
}
