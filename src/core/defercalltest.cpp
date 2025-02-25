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
#include "eventloop.h"

class DeferCallTest : public QObject
{
	Q_OBJECT

private:
	// loop_advance should process enough events to cause the calls to run,
	// without sleeping, in order to prove the calls are run immediately
	int runDeferCall(std::function<void ()> loop_advance)
	{
		int count = 0;

		DeferCall::global()->defer([&] {
			++count;

			DeferCall::global()->defer([&] {
				++count;
			});
		});

		loop_advance();

		return count;
	}

private slots:
	void deferCall()
	{
		EventLoop loop(1);

		int count = runDeferCall([&] {
			// run the first call and queue the second
			loop.step();

			// run the second
			loop.step();
		});

		QCOMPARE(count, 2);

		DeferCall::cleanup();
	}

	void deferCallQt()
	{
		Timer::init(1);

		int count = runDeferCall([] {
			// the underlying timer's qt-based implementation will process
			// both timeouts during a single timer processing pass.
			// therefore, both calls should run within a single event loop
			// pass
			QCoreApplication::processEvents(QEventLoop::AllEvents);
		});

		QCOMPARE(count, 2);

		DeferCall::cleanup();
		Timer::deinit();
	}

	void retract()
	{
		Timer::init(1);

		bool called = false;

		{
			DeferCall deferCall;

			deferCall.defer([&] {
				called = true;
			});
		}

		DeferCall::cleanup();
		QVERIFY(!called);

		Timer::deinit();
	}

	void managerCleanup()
	{
		Timer::init(1);

		int count = 0;

		DeferCall::global()->defer([&] {
			++count;

			DeferCall::global()->defer([&] {
				++count;
			});
		});

		// cleanup should process deferred calls queued so far as well as
		// those queued during processing
		DeferCall::cleanup();
		QCOMPARE(count, 2);

		Timer::deinit();
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
