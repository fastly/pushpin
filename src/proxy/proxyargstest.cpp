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


 #include "config.h"
 #include "app.h"
 #include "settings.h"
 #include "test.h"
 #include "proxyargsdata.h"
 #include "log.h"
 
 void proxyargstest()
 {
    // Create dummy argc/argv for QCoreApplication
	    int argc = 1;
		char appName[] = "pushpin-handler";
		char* argv[] = { appName, nullptr };

    QCoreApplication qapp(argc, argv);
    App app;

    // Get file for example config
    std::string configFile = "examples/config/pushpin.conf";

    // Create test routes array
    const char* route1 = "route1";
    const char* route2 = "route2"; 
    const char* routes[] = { route1, route2 };

    ffi::CliArgsFfi argsFfi = {
        const_cast<char*>(configFile.c_str()),  // config_file
        const_cast<char*>("log.txt"),           // log_file
        3,                                      // log_level
        const_cast<char*>("ipc:prefix"),        // ipc_prefix
        81,                                     // port_offset
        const_cast<char**>(routes),             // routes
        2,                                      // routes_count
        1                                       // quiet_check
    };
 
    // Verify ProxyArgsData parsing
    ProxyArgsData args(&argsFfi);
    TEST_ASSERT_EQ(args.configFile, QString("examples/config/pushpin.conf"));
    TEST_ASSERT_EQ(args.logFile, QString("log.txt"));
    TEST_ASSERT_EQ(args.logLevel, 3);
    TEST_ASSERT_EQ(args.ipcPrefix, QString("ipc:prefix"));
    TEST_ASSERT_EQ(args.portOffset, 81);
    TEST_ASSERT_EQ(args.routeLines, QStringList({"route1", "route2"}));
    TEST_ASSERT_EQ(args.quietCheck, true);

    Settings settings(args.configFile);
    if (!args.ipcPrefix.isEmpty()) settings.setIpcPrefix(args.ipcPrefix);
    if (args.portOffset != -1) settings.setPortOffset(args.portOffset);

    // Test command-line overrides were applied
    TEST_ASSERT_EQ(settings.getPortOffset(), 81);
    TEST_ASSERT_EQ(settings.getIpcPrefix(), QString("ipc:prefix"));

    // Create empty routes array for testing
    static const char* routesEmpty[] = {};

    // Set up valid empty command line arguments
    ffi::CliArgsFfi argsFfiEmpty = {
        const_cast<char*>(configFile.c_str()),  // config_file
        const_cast<char*>(""),                  // log_file
        2,                                      // log_level
        const_cast<char*>(""),                  // ipc_prefix
        -1,                                     // port_offset
        const_cast<char**>(routesEmpty),        // routes array
        0,                                      // routes_count
        0                                       // quiet_check
    };

    // Verify ProxyArgsData parsing with empty arguments
    ProxyArgsData argsEmpty(&argsFfiEmpty);
    TEST_ASSERT_EQ(argsEmpty.configFile, QString("examples/config/pushpin.conf"));
    TEST_ASSERT_EQ(argsEmpty.logFile, QString(""));
    TEST_ASSERT_EQ(argsEmpty.logLevel, 2);
    TEST_ASSERT_EQ(argsEmpty.ipcPrefix, QString(""));
    TEST_ASSERT_EQ(argsEmpty.portOffset, -1);
    TEST_ASSERT_EQ(argsEmpty.routeLines, QStringList());
    TEST_ASSERT_EQ(argsEmpty.quietCheck, false);

    Settings settingsEmpty(argsEmpty.configFile);
    if (!argsEmpty.ipcPrefix.isEmpty()) settingsEmpty.setIpcPrefix(argsEmpty.ipcPrefix);
    if (argsEmpty.portOffset != -1) settingsEmpty.setPortOffset(argsEmpty.portOffset);

    // Test that no overrides were applied (should use config file defaults)
    TEST_ASSERT_EQ(settingsEmpty.getPortOffset(), 0);
    TEST_ASSERT_EQ(settingsEmpty.getIpcPrefix(), QString("pushpin-")); 
 }
 
 extern "C" int proxyargs_test(ffi::TestException *out_ex)
 {
     TEST_CATCH(proxyargstest());
 
     return 0;
 }