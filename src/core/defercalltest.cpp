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

#include <QtTest/QtTest>
#include "timer.h"
#include "defercall.h"

class DeferCallTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		Timer::init(100);
	}

	void cleanupTestCase()
	{
		DeferCall::cleanup();
		Timer::deinit();
	}

	void deferCall()
	{
		bool first = false;
		bool second = false;

		DeferCall::global()->defer([&] {
			first = true;

			DeferCall::global()->defer([&] {
				second = true;
			});
		});

		// the second deferred call should cause the underlying timer to
		// re-arm. since we aren't contending with other timers within this
		// test, both timeouts should get processed during a single timer
		// processing pass. therefore, both calls should get processed within
		// a single eventloop pass
		QTest::qWait(10);
		QVERIFY(first);
		QVERIFY(second);
	}

	void retract()
	{
		bool called = false;

		{
			DeferCall deferCall;

			deferCall.defer([&] {
				called = true;
			});
		}

		DeferCall::cleanup();
		QVERIFY(!called);
	}

	void managerCleanup()
	{
		bool first = false;
		bool second = false;

		DeferCall::global()->defer([&] {
			first = true;

			DeferCall::global()->defer([&] {
				second = true;
			});
		});

		// cleanup should process deferred calls queued so far as well as
		// those queued during processing
		DeferCall::cleanup();
		QVERIFY(first);
		QVERIFY(second);
	}
};

namespace {
namespace Main {
QTEST_MAIN(DeferCallTest)
}
}

extern "C" {

int defercall_test(int argc, char **argv)
{
	return Main::main(argc, argv);
}

}

#include "defercalltest.moc"
