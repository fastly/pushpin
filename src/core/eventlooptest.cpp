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
#include <QtTest/QtTest>
#include <boost/signals2.hpp>
#include "defercall.h"
#include "eventloop.h"
#include "socketnotifier.h"
#include "timer.h"

class EventLoopTest : public QObject
{
	Q_OBJECT

private slots:
	void socketNotifier()
	{
		EventLoop loop(1);

		int fds[2];
		QCOMPARE(pipe(fds), 0);

		SocketNotifier *sn = new SocketNotifier(fds[0], SocketNotifier::Read);

		int activatedFd = -1;
		sn->activated.connect([&](int fd) {
			activatedFd = fd;
			loop.exit(123);
		});

		unsigned char c = 1;
		QCOMPARE(write(fds[1], &c, 1), 1);

		QCOMPARE(loop.exec(), 123);
		QCOMPARE(activatedFd, fds[0]);

		delete sn;
		close(fds[1]);
		close(fds[0]);
	}

	void timer()
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

		QCOMPARE(loop.exec(), 123);
		QCOMPARE(timeoutCount, 2);

		delete t2;
		delete t1;
	}
};

namespace {
namespace Main {
QTEST_MAIN(EventLoopTest)
}
}

extern "C" {

int eventloop_test(int argc, char **argv)
{
	return Main::main(argc, argv);
}

}

#include "eventlooptest.moc"
