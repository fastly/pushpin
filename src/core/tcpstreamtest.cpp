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
#include <QHostAddress>
#include <QTcpSocket>
#include "timer.h"
#include "defercall.h"
#include "tcplistener.h"
#include "tcpstream.h"

class TcpStreamTest : public QObject
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

	void accept()
	{
		TcpListener l;
		QVERIFY(l.bind(QHostAddress("127.0.0.1"), 0));

		auto [addr, port] = l.localAddress();

		bool streamsReady = false;
		l.streamsReady.connect([&] {
			streamsReady = true;
		});

		std::unique_ptr<TcpStream> s = l.accept();
		QVERIFY(!s);

		QTcpSocket client;
		client.connectToHost(addr, port);

		while(!streamsReady)
			QTest::qWait(10);

		s = l.accept();
		QVERIFY(s);

		client.waitForConnected(-1);

		s.reset();
	}
};

namespace {
namespace Main {
QTEST_MAIN(TcpStreamTest)
}
}

extern "C" {

int tcpstream_test(int argc, char **argv)
{
	return Main::main(argc, argv);
}

}

#include "tcpstreamtest.moc"
