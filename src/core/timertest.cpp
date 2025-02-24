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

class TimerTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		Timer::init(100);
	}

	void cleanupTestCase()
	{
		Timer::deinit();
	}

	void zeroTimeout()
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

		// since we aren't contending with other timers in this test, both
		// timeouts should get processed during a single timer processing
		// pass. therefore, both calls should get processed within a single
		// eventloop pass
		QTest::qWait(10);
		QCOMPARE(count, 2);
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
