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
#include "routesfile.h"

static void lineTests()
{
	QList<RoutesFile::RouteSection> r;
	bool ok;

	r = RoutesFile::parseLine("apple", &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(r.count(), 1);
	TEST_ASSERT_EQ(r[0].value, QString("apple"));
	TEST_ASSERT(r[0].props.isEmpty());

	r = RoutesFile::parseLine("apple banana", &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(r.count(), 2);
	TEST_ASSERT_EQ(r[0].value, QString("apple"));
	TEST_ASSERT(r[0].props.isEmpty());
	TEST_ASSERT_EQ(r[1].value, QString("banana"));
	TEST_ASSERT(r[1].props.isEmpty());

	r = RoutesFile::parseLine("  apple   banana  # comment", &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(r.count(), 2);
	TEST_ASSERT_EQ(r[0].value, QString("apple"));
	TEST_ASSERT(r[0].props.isEmpty());
	TEST_ASSERT_EQ(r[1].value, QString("banana"));
	TEST_ASSERT(r[1].props.isEmpty());

	r = RoutesFile::parseLine("apple,organic,type=gala,from=\"washington, \\\"usa\\\"\"", &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(r.count(), 1);
	TEST_ASSERT_EQ(r[0].value, QString("apple"));
	TEST_ASSERT_EQ(r[0].props.count(), 3);
	TEST_ASSERT(r[0].props.contains("organic"));
	TEST_ASSERT(r[0].props.value("organic").isEmpty());
	TEST_ASSERT_EQ(r[0].props.value("type"), QString("gala"));
	TEST_ASSERT_EQ(r[0].props.value("from"), QString("washington, \"usa\""));

	r = RoutesFile::parseLine("apple,organic banana cherry,type=bing", &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(r.count(), 3);
	TEST_ASSERT_EQ(r[0].value, QString("apple"));
	TEST_ASSERT_EQ(r[0].props.count(), 1);
	TEST_ASSERT(r[0].props.contains("organic"));
	TEST_ASSERT(r[0].props.value("organic").isEmpty());
	TEST_ASSERT_EQ(r[1].value, QString("banana"));
	TEST_ASSERT(r[1].props.isEmpty());
	TEST_ASSERT_EQ(r[2].value, QString("cherry"));
	TEST_ASSERT_EQ(r[2].props.value("type"), QString("bing"));

	r = RoutesFile::parseLine(",organic", &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(r.count(), 1);
	TEST_ASSERT_EQ(r[0].value, QString(""));
	TEST_ASSERT_EQ(r[0].props.count(), 1);
	TEST_ASSERT(r[0].props.contains("organic"));
	TEST_ASSERT(r[0].props.value("organic").isEmpty());

	r = RoutesFile::parseLine("type=gala", &ok);
	TEST_ASSERT(ok);
	TEST_ASSERT_EQ(r.count(), 1);
	TEST_ASSERT_EQ(r[0].value, QString(""));
	TEST_ASSERT_EQ(r[0].props.count(), 1);
	TEST_ASSERT(r[0].props.contains("type"));
	TEST_ASSERT_EQ(r[0].props.value("type"), QString("gala"));

	// Unterminated quote
	r = RoutesFile::parseLine("apple,organic,type=\"gala", &ok);
	TEST_ASSERT(!ok);

	// Empty prop name
	r = RoutesFile::parseLine("apple,organic,", &ok);
	TEST_ASSERT(!ok);

	// Empty prop name
	r = RoutesFile::parseLine("apple,organic,=gala", &ok);
	TEST_ASSERT(!ok);
}

extern "C" int routesfile_test(ffi::TestException *out_ex)
{
	TEST_CATCH(lineTests());

	return 0;
}
