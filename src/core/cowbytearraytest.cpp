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

#include <string.h>
#include "test.h"
#include "cowbytearray.h"

static void constructors()
{
	// default
	CowByteArray a;
	TEST_ASSERT(a.isEmpty());

	// char*
	CowByteArray b("hello");
	TEST_ASSERT_EQ(b, "hello");

	// copy
	CowByteArray c(b);
	TEST_ASSERT_EQ(c, "hello");

	// const ref
	QByteArray qa("hello");
	CowByteArrayConstRef constRef(qa);
	CowByteArray d(constRef);
	TEST_ASSERT_EQ(d, "hello");

	// ref
	CowByteArrayRef ref(qa);
	CowByteArray e(ref);
	TEST_ASSERT_EQ(e, "hello");

	// QByteArray
	CowByteArray f(qa);
	TEST_ASSERT_EQ(f, "hello");

	// char* with limit
	CowByteArray g("hello", 3);
	TEST_ASSERT_EQ(g, "hel");

	// sized
	CowByteArray h(10, 'a');
	TEST_ASSERT_EQ(h, "aaaaaaaaaa");
}

static void methods()
{
	// isEmpty
	CowByteArray empty;
	TEST_ASSERT(empty.isEmpty());

	// size and length
	CowByteArray a("hello");
	TEST_ASSERT_EQ(a.size(), 5);
	TEST_ASSERT_EQ(a.length(), 5);

	// data (const and non)
	// data is null-terminated but the null is not counted in the size
	TEST_ASSERT_EQ(strcmp(std::as_const(a).data(), "hello"), 0);
	TEST_ASSERT_EQ(strcmp(a.data(), "hello"), 0);

	// indexOf
	TEST_ASSERT_EQ(a.indexOf('l'), 2);
	TEST_ASSERT_EQ(a.indexOf('z'), -1);

	// mid
	CowByteArray b = a.mid(0, 3);
	CowByteArray c = a.mid(3);
	TEST_ASSERT_EQ(b, "hel");
	TEST_ASSERT_EQ(c, "lo");

	// resize down
	a.resize(3);
	TEST_ASSERT_EQ(a, "hel");

	// resize up
	a.resize(10); // extended data is uninitialized!
	TEST_ASSERT_EQ(a.size(), 10);
}

static void operators()
{
	CowByteArray a("hello");
	TEST_ASSERT_EQ(std::as_const(a)[1], 'e');
	TEST_ASSERT_EQ(a[1], 'e');

	a[1] = 'a';
	TEST_ASSERT_EQ(a, "hallo");

	TEST_ASSERT(CowByteArray("hello") == CowByteArray("hello"));
	TEST_ASSERT(CowByteArray("hello") == "hello");
	TEST_ASSERT("hello" == CowByteArray("hello"));

	TEST_ASSERT(CowByteArray("hello") != CowByteArray("world"));
	TEST_ASSERT(CowByteArray("hello") != "world");
	TEST_ASSERT("hello" != CowByteArray("world"));
}

static void conversions()
{
	CowByteArray a("hello");
	TEST_ASSERT_EQ(a.asQByteArray(), "hello");
}

static void listConstructors()
{
	// default
	CowByteArrayList a;
	TEST_ASSERT(a.isEmpty());

	// QList<QByteArray>
	QList<QByteArray> ql;
	ql += "hello";
	ql += "world";
	CowByteArrayList b(ql);
	TEST_ASSERT_EQ(b.count(), 2);
	TEST_ASSERT_EQ(b[0], "hello");
	TEST_ASSERT_EQ(b[1], "world");
}

static void listMethods()
{
	// isEmpty
	CowByteArrayList a;
	TEST_ASSERT(a.isEmpty());

	// count
	a += CowByteArray("hello");
	a += CowByteArray("world");
	TEST_ASSERT_EQ(a.count(), 2);

	// begin/end (const)
	CowByteArrayList::const_iterator cit = std::as_const(a).begin();
	TEST_ASSERT_EQ(*cit, "hello");

	++cit;
	TEST_ASSERT_EQ(*cit, "world");

	++cit;
	TEST_ASSERT_EQ(cit, a.end());

	// begin/end (non-const)
	CowByteArrayList::iterator it = a.begin();
	TEST_ASSERT_EQ(*it, "hello");

	(*it)[1] = 'a';
	TEST_ASSERT_EQ(a[0], "hallo");
}

static void listOperators()
{
	CowByteArrayList a;

	CowByteArrayList b;
	b += "apple";
	b += "banana";

	a += b;
	a += CowByteArray("cherry");
	a += QByteArray("date");
	a += "elderberry";

	const CowByteArrayList &ac = a;
	TEST_ASSERT_EQ(ac.count(), 5);
	TEST_ASSERT_EQ(ac[0], "apple");
	TEST_ASSERT_EQ(ac[1], "banana");
	TEST_ASSERT_EQ(ac[2], "cherry");
	TEST_ASSERT_EQ(ac[3], "date");
	TEST_ASSERT_EQ(ac[4], "elderberry");

	a[2] = "cantaloupe";
	TEST_ASSERT_EQ(a[2], "cantaloupe");
}

static void listConversions()
{
	CowByteArrayList a;
	a += "hello";
	a += "world";

	const QList<QByteArray> &qlc = std::as_const(a.asQByteArrayList());
	TEST_ASSERT_EQ(qlc.count(), 2);
	TEST_ASSERT_EQ(qlc[0], "hello");
	TEST_ASSERT_EQ(qlc[1], "world");

	QList<QByteArray> &ql = a.asQByteArrayList();
	ql += "foo";
	TEST_ASSERT_EQ(a.count(), 3);
	TEST_ASSERT_EQ(a[2], "foo");
}

extern "C" int cowbytearray_test(ffi::TestException *out_ex)
{
	TEST_CATCH(constructors());
	TEST_CATCH(methods());
	TEST_CATCH(operators());
	TEST_CATCH(conversions());
	TEST_CATCH(listConstructors());
	TEST_CATCH(listMethods());
	TEST_CATCH(listOperators());
	TEST_CATCH(listConversions());

	return 0;
}
