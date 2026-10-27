/*
 * Copyright (C) 2026 Fastly, Inc.
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
 */

#include "settings.h"
#include "test.h"
#include "variant.h"

static const char *CONFIG_FILE = "examples/config/pushpin.conf";

static void mainFileValue() {
    Settings settings(CONFIG_FILE);

    TEST_ASSERT(settings.contains("proxy/routesfile"));
    TEST_ASSERT_EQ(settings.value("proxy/routesfile").toString(), QString("routes"));
}

static void includeFileValue() {
    Settings settings(CONFIG_FILE);

    TEST_ASSERT(settings.contains("proxy/connmgr_in_specs"));
    TEST_ASSERT_EQ(settings.valueRaw("proxy/connmgr_in_specs").toString(),
                   QString("ipc://{rundir}/{ipc_prefix}connmgr-out"));
    TEST_ASSERT_EQ(settings.value("proxy/connmgr_in_specs").toString(),
                   QString("ipc://run/pushpin-connmgr-out"));
}

static void listValue() {
    Settings settings(CONFIG_FILE);

    QStringList expected;
    expected << "ipc://run/pushpin-m2zhttp-out" << "ipc://run/pushpin-m2zws-out";
    TEST_ASSERT_EQ(settings.value("proxy/m2a_in_specs").toStringList(), expected);
}

static void emptyValue() {
    Settings settings(CONFIG_FILE);

    TEST_ASSERT(settings.contains("proxy/x_forwarded_for"));
    TEST_ASSERT_EQ(settings.value("proxy/x_forwarded_for").toString(), QString(""));
}

static void missingKey() {
    Settings settings(CONFIG_FILE);

    TEST_ASSERT(!settings.contains("proxy/no_such_key"));
    TEST_ASSERT_EQ(settings.value("proxy/no_such_key", "fallback").toString(),
                   QString("fallback"));
}

static void overrides() {
    Settings settings(CONFIG_FILE);

    TEST_ASSERT_EQ(settings.ipcPrefix(), QString("pushpin-"));
    TEST_ASSERT_EQ(settings.portOffset(), 0);
    TEST_ASSERT_EQ(settings.adjustedPort("handler/push_in_http_port"), 5561);

    settings.setIpcPrefix("test-");
    settings.setPortOffset(1);

    TEST_ASSERT_EQ(settings.value("proxy/connmgr_in_specs").toString(),
                   QString("ipc://run/test-connmgr-out"));
    TEST_ASSERT_EQ(settings.value("handler/push_in_spec").toString(),
                   QString("tcp://127.0.0.1:5561"));
    TEST_ASSERT_EQ(settings.adjustedPort("handler/push_in_http_port"), 5562);
}

extern "C" int settings_test(ffi::TestException *out_ex) {
    TEST_CATCH(mainFileValue());
    TEST_CATCH(includeFileValue());
    TEST_CATCH(listValue());
    TEST_CATCH(emptyValue());
    TEST_CATCH(missingKey());
    TEST_CATCH(overrides());

    return 0;
}
