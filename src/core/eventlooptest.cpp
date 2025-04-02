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

#include <unistd.h>
#include <boost/signals2.hpp>
#include "test.h"
#include "defercall.h"
#include "eventloop.h"
#include "socketnotifier.h"
#include "timer.h"

static void socketNotifier()
{
	EventLoop loop(1);

	int fds[2];
	TEST_ASSERT_EQ(pipe(fds), 0);

	SocketNotifier *sn = new SocketNotifier(fds[0], SocketNotifier::Read);
	sn->clearReadiness(SocketNotifier::Read);

	int activatedFd = -1;
	uint8_t activatedReadiness = -1;
	sn->activated.connect([&](int fd, uint8_t readiness) {
		activatedFd = fd;
		activatedReadiness = readiness;
		loop.exit(123);
	});

	unsigned char c = 1;
	TEST_ASSERT_EQ(write(fds[1], &c, 1), 1);

	TEST_ASSERT_EQ(loop.exec(), 123);
	TEST_ASSERT_EQ(activatedFd, fds[0]);
	TEST_ASSERT_EQ(activatedReadiness, SocketNotifier::Read);

	delete sn;
	close(fds[1]);
	close(fds[0]);
}

static void timer()
{
	EventLoop loop(2);

	Timer *t1 = new Timer;
	Timer *t2 = new Timer;

	int timeoutCount = 0;

	t1->timeout.connect([&] {
		++timeoutCount;
	});

	t2->timeout.connect([&] {
		++timeoutCount;
		loop.exit(123);
	});

	t1->setSingleShot(true);
	t1->start(0);

	t2->setSingleShot(true);
	t2->start(0);

	TEST_ASSERT_EQ(loop.exec(), 123);
	TEST_ASSERT_EQ(timeoutCount, 2);

	delete t2;
	delete t1;
}

static void custom()
{
	class State
	{
	public:
		EventLoop loop;
		uint8_t activatedReadiness;

		State() :
			loop(EventLoop(1)),
			activatedReadiness(-1)
		{
		}
	};

	State state;

	auto [id, sr] = state.loop.registerCustom([](void *ctx, uint8_t readiness) {
		State *state = (State *)ctx;
		state->activatedReadiness = readiness;
		state->loop.exit(123);
	}, (void *)&state);

	TEST_ASSERT(id >= 0);
	TEST_ASSERT_EQ(sr->setReadiness(Event::Readable), 0);
	TEST_ASSERT_EQ(state.loop.exec(), 123);
	TEST_ASSERT_EQ(state.activatedReadiness, Event::Readable);

	state.loop.deregister(id);
}

extern "C" int eventloop_test(ffi::TestException *out_ex)
{
	TEST_CATCH(socketNotifier());
	TEST_CATCH(timer());
	TEST_CATCH(custom());

	return 0;
}
