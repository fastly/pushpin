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

    // Number (long long)
    TEST_ASSERT_EQ(CowString::number(10000000000LL), "10000000000");

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

    // Size / length
    CowString g("hello");
    TEST_ASSERT_EQ(g.size(), 5);
    TEST_ASSERT_EQ(g.length(), 5);

    // Trimmed
    CowString h("  hello  ");
    TEST_ASSERT_EQ(h.trimmed(), "hello");

    // ToLower / toUpper
    CowString i("Hello World");
    TEST_ASSERT_EQ(i.toLower(), "hello world");
    TEST_ASSERT_EQ(i.toUpper(), "HELLO WORLD");

    // Left / right
    CowString j("hello world");
    TEST_ASSERT_EQ(j.left(5), "hello");
    TEST_ASSERT_EQ(j.right(5), "world");

    // EndsWith
    TEST_ASSERT(j.endsWith("world"));
    TEST_ASSERT(!j.endsWith("hello"));

    // IndexOf (string)
    TEST_ASSERT_EQ(j.indexOf("world"), 6);
    TEST_ASSERT_EQ(j.indexOf("xyz"), -1);

    // IndexOf (char)
    TEST_ASSERT_EQ(j.indexOf(' '), 5);
    TEST_ASSERT_EQ(j.indexOf('z'), -1);

    // ToInt
    bool ok;
    TEST_ASSERT_EQ(CowString("42").toInt(&ok), 42);
    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(CowString("ff").toInt(&ok, 16), 255);
    TEST_ASSERT(ok);
    CowString("abc").toInt(&ok);
    TEST_ASSERT(!ok);

    // Asprintf
    CowString k = CowString::asprintf("%s=%d", "x", 7);
    TEST_ASSERT_EQ(k, "x=7");

    // Repeated-char constructor
    CowString l(3, '*');
    TEST_ASSERT_EQ(l, "***");
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

    // Operator+=
    CowString b("hello");
    b += " world";
    TEST_ASSERT_EQ(b, "hello world");
    b += CowString("!");
    TEST_ASSERT_EQ(b, "hello world!");
    b += '?';
    TEST_ASSERT_EQ(b, "hello world!?");

    // Operator< (lexicographic)
    TEST_ASSERT(CowString("apple") < CowString("banana"));
    TEST_ASSERT(!(CowString("banana") < CowString("apple")));
    TEST_ASSERT(!(CowString("hello") < CowString("hello")));
}

static void conversions() {
    CowString a("hello");
    TEST_ASSERT_EQ(a, "hello");
}

static void refConstructors() {
    // CowStringConstRef from const QString&
    QString qs("hello");
    CowStringConstRef constRef(qs);
    TEST_ASSERT(!constRef.isEmpty());
    TEST_ASSERT_EQ(constRef.asQString(), qs);

    // CowString from CowStringConstRef
    CowString a(constRef);
    TEST_ASSERT_EQ(a, "hello");

    // CowStringRef from QString&
    CowStringRef ref(qs);
    TEST_ASSERT(!ref.isEmpty());
    TEST_ASSERT_EQ(ref.asQString(), qs);

    // CowString from CowStringRef
    CowString b(ref);
    TEST_ASSERT_EQ(b, "hello");
}

static void refMethods() {
    QString qs("hello world");
    CowStringConstRef constRef(qs);

    // isEmpty
    QString qsEmpty;
    CowStringConstRef emptyRef(qsEmpty);
    TEST_ASSERT(emptyRef.isEmpty());
    TEST_ASSERT(!constRef.isEmpty());

    // startsWith
    TEST_ASSERT(constRef.startsWith("hello"));
    TEST_ASSERT(!constRef.startsWith("world"));

    // contains
    TEST_ASSERT(constRef.contains("lo wo"));
    TEST_ASSERT(!constRef.contains("xyz"));

    // mid (const ref)
    CowString m1 = constRef.mid(6);
    TEST_ASSERT_EQ(m1, "world");
    CowString m2 = constRef.mid(6, 3);
    TEST_ASSERT_EQ(m2, "wor");

    // CowStringRef operator= writes through to the underlying QString
    CowStringRef ref(qs);
    ref = CowString("goodbye");
    TEST_ASSERT_EQ(qs, "goodbye");

    // mid (mutable ref)
    QString qs2("hello world");
    CowStringRef ref2(qs2);
    CowString m3 = ref2.mid(6);
    TEST_ASSERT_EQ(m3, "world");
}

static void listConstructors() {
    // Default
    CowStringList a;
    TEST_ASSERT(a.isEmpty());

    // QStringList
    QStringList ql;
    ql += "hello";
    ql += "world";
    CowStringList b(ql);
    TEST_ASSERT_EQ(b.count(), 2);
    TEST_ASSERT_EQ(b[0].asQString(), "hello");
    TEST_ASSERT_EQ(b[1].asQString(), "world");
}

static void listMethods() {
    // isEmpty
    CowStringList a;
    TEST_ASSERT(a.isEmpty());

    // count
    a += CowString("hello");
    a += CowString("world");
    TEST_ASSERT_EQ(a.count(), 2);

    // contains
    TEST_ASSERT(a.contains("world"));
    TEST_ASSERT(!a.contains("foo"));

    // begin/end (const)
    CowStringList::const_iterator cit = std::as_const(a).begin();
    TEST_ASSERT_EQ((*cit).asQString(), "hello");

    ++cit;
    TEST_ASSERT_EQ((*cit).asQString(), "world");

    ++cit;
    TEST_ASSERT_EQ(cit, std::as_const(a).end());

    // begin/end (non-const), write through ref
    CowStringList::iterator it = a.begin();
    TEST_ASSERT_EQ((*it).asQString(), "hello");

    *it = CowString("hallo");
    TEST_ASSERT_EQ(a[0].asQString(), "hallo");

    // join
    CowStringList b;
    b += CowString("foo");
    b += CowString("bar");
    b += CowString("baz");
    TEST_ASSERT_EQ(b.join(", "), "foo, bar, baz");
}

static void listOperators() {
    CowStringList a;

    CowStringList b;
    b += "apple";
    b += "banana";

    a += b;
    a += CowString("cherry");
    a += QString("date");
    a += "elderberry";

    const CowStringList &ac = a;
    TEST_ASSERT_EQ(ac.count(), 5);
    TEST_ASSERT_EQ(ac[0].asQString(), "apple");
    TEST_ASSERT_EQ(ac[1].asQString(), "banana");
    TEST_ASSERT_EQ(ac[2].asQString(), "cherry");
    TEST_ASSERT_EQ(ac[3].asQString(), "date");
    TEST_ASSERT_EQ(ac[4].asQString(), "elderberry");

    // mutable operator[] writes through
    a[2] = CowString("cantaloupe");
    TEST_ASSERT_EQ(a[2].asQString(), "cantaloupe");
}

static void listConversions() {
    CowStringList a;
    a += "hello";
    a += "world";

    const QStringList &qlc = std::as_const(a).asQStringList();
    TEST_ASSERT_EQ(qlc.count(), 2);
    TEST_ASSERT_EQ(qlc[0], "hello");
    TEST_ASSERT_EQ(qlc[1], "world");

    QStringList &ql = a.asQStringList();
    ql += "foo";
    TEST_ASSERT_EQ(a.count(), 3);
    TEST_ASSERT_EQ(a[2].asQString(), "foo");
}

extern "C" int cowstring_test(ffi::TestException *out_ex) {
    TEST_CATCH(constructors());
    TEST_CATCH(methods());
    TEST_CATCH(operators());
    TEST_CATCH(conversions());
    TEST_CATCH(refConstructors());
    TEST_CATCH(refMethods());
    TEST_CATCH(listConstructors());
    TEST_CATCH(listMethods());
    TEST_CATCH(listOperators());
    TEST_CATCH(listConversions());

    return 0;
}
