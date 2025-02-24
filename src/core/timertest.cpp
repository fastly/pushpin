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
#include "eventloop.h"

class TimerTest : public QObject
{
	Q_OBJECT

private:
	// loop_advance should process enough events to cause the timers to
	// activate, without sleeping, in order to prove timeouts of zero are
	// processed immediately
	int runZeroTimeout(std::function<void ()> loop_advance)
	{
		Timer t;
		t.setSingleShot(true);

		int count = 0;

		t.timeout.connect([&] {
			++count;
			if(count < 2)
				t.start(0);
		});

		t.start(0);

		loop_advance();

		return count;
	}

private slots:
	void zeroTimeout()
	{
		EventLoop loop(1);

		int count = runZeroTimeout([&] {
			// activate the first timer and queue the second
			loop.step();

			// activate the second
			loop.step();
		});

		QCOMPARE(count, 2);
	}

	void zeroTimeoutQt()
	{
		Timer::init(1);

		int count = runZeroTimeout([] {
			// the timer's qt-based implementation will process both timeouts
			// during a single timer processing pass. therefore, both
			// timeouts should get processed within a single event loop pass
			QCoreApplication::processEvents(QEventLoop::AllEvents);
		});

		QCOMPARE(count, 2);

		Timer::deinit();
	}
};

namespace {
namespace Main {
QTEST_MAIN(TimerTest)
}
}

extern "C" {

int timer_test(int argc, char **argv)
{
	return Main::main(argc, argv);
}

}

#include "timertest.moc"
