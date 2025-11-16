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

#include "test.h"
#include "timer.h"
#include "eventloop.h"

// loop_advance should process enough events to cause the timers to
// Activate, without sleeping, in order to prove timeouts of zero are
// processed immediately
static int runZeroTimeout(std::function<void ()> loop_advance)
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

static void zeroTimeout()
{
	EventLoop loop(1);

	int count = runZeroTimeout([&] {
		// Activate the first timer and queue the second
		loop.step();

		// Activate the second
		loop.step();
	});

	TEST_ASSERT_EQ(count, 2);
}

extern "C" int timer_test(ffi::TestException *out_ex)
{
	TEST_CATCH(zeroTimeout());

	return 0;
}
