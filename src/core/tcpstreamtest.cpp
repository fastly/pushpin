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

#include <QCoreApplication>
#include <QHostAddress>
#include "test.h"
#include "timer.h"
#include "defercall.h"
#include "eventloop.h"
#include "tcplistener.h"
#include "tcpstream.h"

static void runAccept(std::function<void ()> loop_wait)
{
	TcpListener l;
	TEST_ASSERT(l.bind(QHostAddress("127.0.0.1"), 0));

	auto [addr, port] = l.localAddress();

	// start by assuming operations are possible
	bool streamsReady = true;

	l.streamsReady.connect([&] {
		streamsReady = true;
	});

	std::unique_ptr<TcpStream> s = l.accept();
	TEST_ASSERT(!s);
	TEST_ASSERT_EQ(l.errorCondition(), EAGAIN);
	streamsReady = false;

	TcpStream client;
	TEST_ASSERT(client.connect(addr, port));

	// start by assuming operations are possible
	bool clientWriteReady = true;

	client.writeReady.connect([&] {
		clientWriteReady = true;
	});

	while(!streamsReady)
		loop_wait();

	s = l.accept();
	TEST_ASSERT(s);

	while(!client.checkConnected())
	{
		TEST_ASSERT_EQ(client.errorCondition(), ENOTCONN);

		clientWriteReady = false;
		while(!clientWriteReady)
			loop_wait();
	}
}

static void runIo(std::function<void ()> loop_wait)
{
	TcpListener l;
	TEST_ASSERT(l.bind(QHostAddress("127.0.0.1"), 0));

	auto [addr, port] = l.localAddress();

	// start by assuming operations are possible
	bool streamsReady = true;

	l.streamsReady.connect([&] {
		streamsReady = true;
	});

	TcpStream client;
	TEST_ASSERT(client.connect(addr, port));

	// start by assuming operations are possible
	bool clientReadReady = true;
	bool clientWriteReady = true;

	client.readReady.connect([&] {
		clientReadReady = true;
	});

	client.writeReady.connect([&] {
		clientWriteReady = true;
	});

	std::unique_ptr<TcpStream> s;
	while(!s)
	{
		s = l.accept();

		if(!s)
		{
			TEST_ASSERT_EQ(l.errorCondition(), EAGAIN);

			streamsReady = false;
			while(!streamsReady)
				loop_wait();
		}
	}

	// start by assuming operations are possible
	bool readReady = true;
	bool writeReady = true;

	s->readReady.connect([&] {
		readReady = true;
	});

	s->writeReady.connect([&] {
		writeReady = true;
	});

	while(!client.checkConnected())
	{
		TEST_ASSERT_EQ(client.errorCondition(), ENOTCONN);

		clientWriteReady = false;
		while(!clientWriteReady)
			loop_wait();
	}

	TEST_ASSERT(s->read().isNull());
	TEST_ASSERT_EQ(s->errorCondition(), EAGAIN);
	readReady = false;

	TEST_ASSERT_EQ(client.write("hello\n"), 6);

	QByteArray received;
	while(!received.contains('\n'))
	{
		QByteArray buf = s->read();

		if(buf.isNull())
		{
			TEST_ASSERT_EQ(s->errorCondition(), EAGAIN);

			readReady = false;
			while(!readReady)
				loop_wait();

			continue;
		}

		TEST_ASSERT(!buf.isEmpty());

		received += buf;
	}

	TEST_ASSERT_EQ(received, "hello\n");

	QByteArray written;
	received.clear();

	// write until we fill the system buffer
	while(true)
	{
		QByteArray chunk(100000, 'a');
		int ret = s->write(chunk);

		if(ret < 0)
		{
			TEST_ASSERT_EQ(s->errorCondition(), EAGAIN);
			writeReady = false;
			break;
		}

		written += chunk.mid(0, ret);
	}

	// wait for some bytes on the client side
	while(received.isEmpty())
	{
		QByteArray buf = client.read(100000);

		if(buf.isNull())
		{
			TEST_ASSERT_EQ(client.errorCondition(), EAGAIN);

			clientReadReady = false;
			while(!clientReadReady)
				loop_wait();

			continue;
		}

		received += buf;
	}

	// now read as much as possible on the client side. this helps the
	// server side gain writability sooner
	while(true)
	{
		QByteArray buf = client.read(100000);

		if(buf.isNull())
		{
			TEST_ASSERT_EQ(client.errorCondition(), EAGAIN);
			clientReadReady = false;
			break;
		}

		received += buf;
	}

	// wait for writability
	while(!writeReady)
		loop_wait();

	// write more
	{
		QByteArray chunk(100000, 'a');
		int ret = s->write(chunk);
		TEST_ASSERT(ret > 0);

		written += chunk.mid(0, ret);
	}

	// close the server side
	s.reset();

	// read until closed on the client side
	while(true)
	{
		QByteArray buf = client.read(100000);

		if(buf.isNull())
		{
			TEST_ASSERT_EQ(client.errorCondition(), EAGAIN);

			clientReadReady = false;
			while(!clientReadReady)
				loop_wait();

			continue;
		}

		if(buf.isEmpty())
			break;

		received += buf;
	}

	TEST_ASSERT_EQ(received, written);
}

static void accept()
{
	EventLoop loop(100);

	runAccept([&] {
		QThread::msleep(10);
		loop.step();
	});

	DeferCall::cleanup();
}

static void acceptQt()
{
	int argc = 1;
	char *argv[] = { "zeroTimeoutQt" };
	QCoreApplication qapp(argc, argv);
	Timer::init(100);

	runAccept([] { QTest::qWait(10); });

	DeferCall::cleanup();
	Timer::deinit();
}

static void io()
{
	EventLoop loop(100);

	runIo([&] {
		QThread::msleep(10);
		loop.step();
	});

	DeferCall::cleanup();
}

static void ioQt()
{
	int argc = 1;
	char *argv[] = { "zeroTimeoutQt" };
	QCoreApplication qapp(argc, argv);
	Timer::init(100);

	runIo([] { QTest::qWait(10); });

	DeferCall::cleanup();
	Timer::deinit();
}

extern "C" int tcpstream_test(ffi::TestException *out_ex)
{
	TEST_CATCH(accept());
	TEST_CATCH(acceptQt());
	TEST_CATCH(io());
	TEST_CATCH(ioQt());

	return 0;
}
