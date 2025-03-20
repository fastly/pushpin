/*
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

#include "string.h"
#include <QtTest/QtTest>
#include "rust/bindings.h"

#ifndef PUSHPIN_TEST_H
#define PUSHPIN_TEST_H

class TestException
{
public:
    std::string file;
    int line;
    std::string message;

    TestException(const std::string &file, int line, const std::string &message) :
        file(file),
        line(line),
        message(message)
    {
    }

    void toFfi(ffi::TestException *dest) const
    {
        ffi::test_exception_set(dest, file.c_str(), line, message.c_str());
    }
};

inline void test_assert(bool cond, const char *condStr, const char *file, int line)
{
    if(!cond)
        throw TestException(file, line, std::string("assertion failed: ") + condStr);
}

// uses QtTest to stringify values
template <typename T1, typename T2>
inline void test_assert_eq(const T1 &left, const T2 &right, const char *file, int line)
{
    if(!(left == right))
        throw TestException(file, line, std::string("assertion `left == right` failed\n  left: ") + QTest::toString(left) + "\n right: " + QTest::toString(right));
}

// if cond is false, throws an exception with similar message as rust's assert macro
#define TEST_ASSERT(cond) \
do {\
    test_assert(static_cast<bool>(cond), #cond, __FILE__, __LINE__);\
} while (false)

// if left != right, throws an exception with similar message as rust's assert_eq macro
#define TEST_ASSERT_EQ(left, right) \
do {\
    test_assert_eq(left, right, __FILE__, __LINE__);\
} while (false)

// for running a test and catching an exception if any. expects local variable ffi::TestException* out_ex to exist
#define TEST_CATCH(statement) try { statement; } catch(const TestException &ex) { ex.toFfi(out_ex); return 1; }

class TestQCoreApplication
{
public:
    TestQCoreApplication()
    {
        argc_ = 1;
        argv_[0] = strdup("qt-test");
        qapp_ = new QCoreApplication(argc_, argv_);
    }

    ~TestQCoreApplication()
    {
        delete qapp_;
        free(argv_[0]);
    }

private:
    int argc_;
    char *argv_[1];
    QCoreApplication *qapp_;
};

#endif
