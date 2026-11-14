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

#include "cowstring.h"
#include "test.h"
#include <cstring>

static void constructors() {
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

static void methods() {
    // IsEmpty
    CowString empty;
    TEST_ASSERT(empty.isEmpty());

    // Clear
    CowString a("hello");
    TEST_ASSERT(!a.isEmpty());
    a.clear();
    TEST_ASSERT(a.isEmpty());

    // StartsWith
    CowString b("hello world");
    TEST_ASSERT(b.startsWith("hello"));
    TEST_ASSERT(!b.startsWith("world"));

    // Contains
    TEST_ASSERT(b.contains("lo wo"));
    TEST_ASSERT(!b.contains("xyz"));

    // Mid
    TEST_ASSERT_EQ(b.mid(6), "world");
    TEST_ASSERT_EQ(b.mid(6, 3), "wor");

    // Arg (single)
    CowString tmpl1("hello %1");
    TEST_ASSERT_EQ(tmpl1.arg("world"), "hello world");

    // Arg (two)
    CowString tmpl2("%1 %2");
    TEST_ASSERT_EQ(tmpl2.arg("hello", "world"), "hello world");

    // Number (int)
    TEST_ASSERT_EQ(CowString::number(42), "42");
    TEST_ASSERT_EQ(CowString::number(255, 16), "ff");

    // Number (double)
    TEST_ASSERT_EQ(CowString::number(1.5, 'f', 1), "1.5");

    // ToUtf8
    CowString c("hello");
    CowByteArray ba = c.toUtf8();
    TEST_ASSERT_EQ(ba, "hello");

    // FromUtf8
    CowByteArray utf8("hello");
    CowString d = CowString::fromUtf8(utf8);
    TEST_ASSERT_EQ(d, "hello");

    // FromUtf8 round-trip
    CowString e("world");
    TEST_ASSERT_EQ(CowString::fromUtf8(e.toUtf8()), "world");

    // cowPrintable
    CowString f("test");
    TEST_ASSERT(strcmp(cowPrintable(f), "test") == 0);
}

static void operators() {
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

    // Operator+
    TEST_ASSERT_EQ(CowString("hello") + " " + "world", "hello world");
}

static void conversions() {
    CowString a("hello");
    TEST_ASSERT_EQ(a, "hello");
}

extern "C" int cowstring_test(ffi::TestException *out_ex) {
    TEST_CATCH(constructors());
    TEST_CATCH(methods());
    TEST_CATCH(operators());
    TEST_CATCH(conversions());

    return 0;
}
