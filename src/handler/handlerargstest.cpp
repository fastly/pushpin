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

void handlerargstest()
{
    // Get file for example config
    std::string configFile = "examples/config/pushpin.conf";

    // Set up the command line arguments
    int argc = 5;
    char * argv[] = {
        (char *) configFile.c_str(), // configFile
        (char *) "log.txt",          // logFile
        (char *) "2",                // logLevel
        (char *) "ipc:prefix",       // ipcPrefix
        (char *) "80"              // portOffset
    };

    // Set up the QCoreApplication
    QCoreApplication qapp(argc, argv);
    HandlerApp app;
    QStringList args = qapp.arguments();
    
    // Test the arguments
    TEST_ASSERT_EQ(args[0], QString("examples/config/pushpin.conf"));
    TEST_ASSERT_EQ(args[1], QString("log.txt"));
    TEST_ASSERT_EQ(args[2], QString("2"));
    TEST_ASSERT_EQ(args[3], QString("ipc:prefix"));
    TEST_ASSERT_EQ(args[4], QString("80"));

    // Set up the correct mock settings
    Settings mock_settings(args[0]);
    mock_settings.setIpcPrefix(args[3]);
    mock_settings.setPortOffset(args[4].toInt());

    // Load settings from command line arguments
    Settings settings = app.loadSettingsFromCliArgs();

    // This should match
    TEST_ASSERT_EQ(mock_settings, settings);

    // Change the port offset to a different value
    mock_settings.setPortOffset(60);

    // This shouldn't match
    TEST_ASSERT(!(mock_settings == settings));
}

extern "C" int handlerargs_test(ffi::TestException *out_ex)
{
	TEST_CATCH(handlerargstest());

	return 0;
}