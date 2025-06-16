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
#include "handlerapp.h"
#include "settings.h"
#include "test.h"
#include "argsdata.h"

void handlerargstest()
{
    // Get file for example config
    std::string configFile = "examples/config/pushpin.conf";

    // Set up valid command line arguments
    int argc = 7;
    char * argv[] = {
        (char *) configFile.c_str(), // configFile
        (char *) "log.txt",          // logFile
        (char *) "2",                // logLevel
        (char *) "ipc:prefix",       // ipcPrefix
        (char *) "80",               // portOffset
        (char *) "route1,route2",    // routeLines
        (char *) "true",             // quietCheck
    };

    // Set up QCoreApplication
    QCoreApplication qapp(argc, argv);
    HandlerApp app;
    QStringList extArgs = qapp.arguments();
    
    // Verify the arguments
    TEST_ASSERT_EQ(extArgs[0], QString("examples/config/pushpin.conf"));
    TEST_ASSERT_EQ(extArgs[1], QString("log.txt"));
    TEST_ASSERT_EQ(extArgs[2], QString("2"));
    TEST_ASSERT_EQ(extArgs[3], QString("ipc:prefix"));
    TEST_ASSERT_EQ(extArgs[4], QString("80"));
    TEST_ASSERT_EQ(extArgs[5], QString("route1,route2"));
    TEST_ASSERT_EQ(extArgs[6], QString("true"));

    // Verify ArgsData parsing
    ArgsData args(extArgs);
    TEST_ASSERT_EQ(args.configFile, QString("examples/config/pushpin.conf"));
    TEST_ASSERT_EQ(args.logFile, QString("log.txt"));
    TEST_ASSERT_EQ(args.logLevel, 2);
    TEST_ASSERT_EQ(args.ipcPrefix, QString("ipc:prefix"));
    TEST_ASSERT_EQ(args.portOffset, 80);
    TEST_ASSERT_EQ(args.routeLines, QStringList({"route1", "route2"}));
    TEST_ASSERT_EQ(args.quietCheck, true);

    // Set up mock settings
    Settings mock_settings(args.configFile);
    if (!args.ipcPrefix.isEmpty())
        mock_settings.setIpcPrefix(args.ipcPrefix);;
    if (args.portOffset != -1)
        mock_settings.setPortOffset(args.portOffset);

    // Load settings from command line arguments
    Settings settings = args.loadIntoSettings();

    // Verify mock settings match the loaded settings
    TEST_ASSERT_EQ(mock_settings, settings);

    // Change the port offset to a different value
    mock_settings.setPortOffset(60);

    // Verify that the mock settings no longer match the loaded settings
    TEST_ASSERT(!(mock_settings == settings));

    // Set up valid empty command line arguments
    int argc_empty = 7;
    char * argv_empty[] = {
        (char *) configFile.c_str(), // configFile
        (char *) "",                 // logFile
        (char *) "2",                // logLevel
        (char *) "",                 // ipcPrefix
        (char *) "",                 // portOffset
        (char *) "",                 // routeLines
        (char *) "false",            // quietCheck
    };

    // Set up QCoreApplication with empty arguments
    QCoreApplication qapp_empty(argc_empty, argv_empty);
    HandlerApp app_empty;
    QStringList extArgs_empty = qapp_empty.arguments();
    
    // Verify the arguments
    TEST_ASSERT_EQ(extArgs[0], QString("examples/config/pushpin.conf"));
    TEST_ASSERT_EQ(extArgs[1], QString(""));
    TEST_ASSERT_EQ(extArgs[2], QString("2"));
    TEST_ASSERT_EQ(extArgs[3], QString(""));
    TEST_ASSERT_EQ(extArgs[4], QString(""));
    TEST_ASSERT_EQ(extArgs[5], QString(""));
    TEST_ASSERT_EQ(extArgs[6], QString("false"));

    // Verify ArgsData parsing with empty arguments
    ArgsData args_empty(extArgs_empty);
    TEST_ASSERT_EQ(args_empty.configFile, QString("examples/config/pushpin.conf"));
    TEST_ASSERT_EQ(args_empty.logFile, QString(""));
    TEST_ASSERT_EQ(args_empty.logLevel, 2);
    TEST_ASSERT_EQ(args_empty.ipcPrefix, QString(""));
    TEST_ASSERT_EQ(args_empty.portOffset, -1);
    TEST_ASSERT_EQ(args_empty.routeLines, QStringList());
    TEST_ASSERT_EQ(args_empty.quietCheck, false);

    // Load settings from empty command line arguments
    Settings settings_empty = args_empty.loadIntoSettings();

    // Set up mock settings
    Settings mock_settings_empty(args_empty.configFile);
    if (!args_empty.ipcPrefix.isEmpty())
        mock_settings_empty.setIpcPrefix(args_empty.ipcPrefix);
    if (args_empty.portOffset != -1)
        mock_settings_empty.setPortOffset(args_empty.portOffset);
    
    // Verify mock settings match the loaded settings
    TEST_ASSERT_EQ(mock_settings_empty, settings_empty);
}

extern "C" int handlerargs_test(ffi::TestException *out_ex)
{
	TEST_CATCH(handlerargstest());

	return 0;
}