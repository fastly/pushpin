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

#include <thread>
#include "test.h"
#include "defercall.h"
#include "eventloop.h"

// loop_advance should process enough events to cause the calls to run,
// without sleeping, in order to prove the calls are run immediately
static std::tuple<int, int> runDeferCall(std::function<void ()> loop_advance)
{
	DeferCall deferCall;
	int count = 0;

	deferCall.defer([&] {
		++count;

		deferCall.defer([&] {
			++count;
		});
	});

	loop_advance();

	return {deferCall.pendingCount(), count};
}

// spawns a thread, triggers the deferCall from it, then waits for thread to
// finish
static void callNonLocal(DeferCall *deferCall, std::function<void ()> handler)
{
	std::thread thread([=] {
		deferCall->defer(handler);
	});
	thread.join();
}

// loop_advance should process enough events to cause the calls to run,
// without sleeping, in order to prove the calls are run immediately
static std::tuple<int, int> runNonLocal(std::function<void ()> loop_advance)
{
	DeferCall deferCall;
	int count = 0;

	callNonLocal(&deferCall, [&] {
		++count;
	});

	loop_advance();

	return {deferCall.pendingCount(), count};
}

static void deferCall()
{
	EventLoop loop(2);

	auto [pendingCount, count] = runDeferCall([&] {
		// run the first call and queue the second
		loop.step();

		// run the second
		loop.step();
	});

	TEST_ASSERT_EQ(pendingCount, 0);
	TEST_ASSERT_EQ(count, 2);
}

static void nonLocal()
{
	EventLoop loop(2);

	auto [pendingCount, count] = runNonLocal([&] {
		// run the first call
		loop.step();
	});

	TEST_ASSERT_EQ(pendingCount, 0);
	TEST_ASSERT_EQ(count, 1);
}

static void retract()
{
	EventLoop loop(2);

	bool called = false;

	{
		DeferCall deferCall;

		deferCall.defer([&] {
			called = true;
		});
	}

	DeferCall::cleanup();
	TEST_ASSERT(!called);
}

static void managerCleanup()
{
	EventLoop loop(2);

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
	TEST_ASSERT_EQ(count, 2);
}

extern "C" int defercall_test(ffi::TestException *out_ex)
{
	TEST_CATCH(deferCall());
	TEST_CATCH(nonLocal());
	TEST_CATCH(retract());
	TEST_CATCH(managerCleanup());

	return 0;
}
