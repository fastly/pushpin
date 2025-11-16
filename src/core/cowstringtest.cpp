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
#include "cowstring.h"

static void constructors()
{
	// Default
	CowString a;
	TEST_ASSERT(a.isEmpty());

	// Char*
	CowString b("hello");
	TEST_ASSERT_EQ(b, "hello");

	// Copy
	CowString c(b);
	TEST_ASSERT_EQ(c, "hello");

	// QString
	QString qs("hello");
	CowString d(qs);
	TEST_ASSERT_EQ(d, "hello");
}

static void methods()
{
	// IsEmpty
	CowString empty;
	TEST_ASSERT(empty.isEmpty());

	// Clear
	CowString a("hello");
	TEST_ASSERT(!a.isEmpty());
	a.clear();
	TEST_ASSERT(a.isEmpty());

	// ToUtf8
	CowString b("hello");
	CowByteArray ba = b.toUtf8();
	TEST_ASSERT_EQ(ba, "hello");
}

static void operators()
{
	// Operator=
	CowString a;
	a = CowString("hello");
	TEST_ASSERT_EQ(a, "hello");

	// Operator==
	TEST_ASSERT(CowString("hello") == CowString("hello"));
	TEST_ASSERT(CowString("hello") == "hello");
	TEST_ASSERT("hello" == CowString("hello"));

	// Operator!=
	TEST_ASSERT(CowString("hello") != CowString("world"));
	TEST_ASSERT(CowString("hello") != "world");
	TEST_ASSERT("hello" != CowString("world"));
}

static void conversions()
{
	CowString a("hello");
	TEST_ASSERT_EQ(a, "hello");
}

extern "C" int cowstring_test(ffi::TestException *out_ex)
{
	TEST_CATCH(constructors());
	TEST_CATCH(methods());
	TEST_CATCH(operators());
	TEST_CATCH(conversions());

	return 0;
}
