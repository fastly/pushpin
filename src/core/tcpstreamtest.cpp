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
	}

	void io()
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

		// start by assuming operations are possible
		bool readReady = true;
		bool writeReady = true;

		s->readReady.connect([&] {
			readReady = true;
		});

		s->writeReady.connect([&] {
			writeReady = true;
		});

		client.waitForConnected(-1);

		client.write("hello\n");

		QByteArray received;
		while(!received.contains('\n'))
		{
			QByteArray buf = s->read();

			if(buf.isNull())
			{
				QCOMPARE(s->errorCondition(), EAGAIN);

				readReady = false;
				while(!readReady)
					QTest::qWait(10);

				continue;
			}

			if(buf.isEmpty())
				break;

			received += buf;
		}

		QCOMPARE(received, "hello\n");

		QByteArray written;
		received.clear();

		// without running the event loop, write until we fill the system
		// buffer
		while(true)
		{
			QByteArray chunk(100000, 'a');
			int ret = s->write(chunk);

			if(ret < 0)
			{
				QCOMPARE(s->errorCondition(), EAGAIN);
				writeReady = false;
				break;
			}

			written += chunk.mid(0, ret);
		}

		// read some on the client side
		while(received.isEmpty())
		{
			QByteArray buf = client.read(100000);
			if(buf.isEmpty())
			{
				QTest::qWait(10);
				continue;
			}

			received += buf;
		}

		// wait for writability
		while(!writeReady)
			QTest::qWait(10);

		// write more
		{
			QByteArray chunk(100000, 'a');
			int ret = s->write(chunk);
			QVERIFY(ret > 0);

			written += chunk.mid(0, ret);
		}

		// close the server side
		s.reset();

		// read until closed on the client side
		while(true)
		{
			while(client.bytesAvailable())
				received += client.read(100000);

			if(client.state() != QTcpSocket::ConnectedState)
				break;

			QTest::qWait(10);
		}

		QCOMPARE(received, written);
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
