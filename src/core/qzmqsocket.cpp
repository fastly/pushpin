/*
 * Copyright (C) 2012-2020 Justin Karneges
 * Copyright (C) 2024-2025 Fastly, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "qzmqsocket.h"

#include <assert.h>
#include <list>
#include <mutex>
#include <boost/signals2.hpp>
#include "rust/bindings.h"
#include "cowstring.h"
#include "qzmqcontext.h"
#include "timer.h"
#include "socketnotifier.h"

using Connection = boost::signals2::scoped_connection;

namespace QZmq {

static int get_fd(void *sock)
{
	int fd;
	size_t opt_len = sizeof(fd);
	int ret = ffi::wzmq_getsockopt(sock, ffi::WZMQ_FD, &fd, &opt_len);
	assert(ret == 0);
	return fd;
}

static void set_subscribe(void *sock, const char *data, int size)
{
	size_t opt_len = size;
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_SUBSCRIBE, data, opt_len);
	assert(ret == 0);
}

static void set_unsubscribe(void *sock, const char *data, int size)
{
	size_t opt_len = size;
	ffi::wzmq_setsockopt(sock, ffi::WZMQ_UNSUBSCRIBE, data, opt_len);
	// note: we ignore errors, such as unsubscribing a nonexisting filter
}

static void set_linger(void *sock, int value)
{
	size_t opt_len = sizeof(value);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_LINGER, &value, opt_len);
	assert(ret == 0);
}

static int get_identity(void *sock, char *data, int size)
{
	size_t opt_len = size;
	int ret = ffi::wzmq_getsockopt(sock, ffi::WZMQ_IDENTITY, data, &opt_len);
	assert(ret == 0);
	return (int)opt_len;
}

static void set_identity(void *sock, const char *data, int size)
{
	size_t opt_len = size;
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_IDENTITY, data, opt_len);
	assert(ret == 0);
}

static void set_immediate(void *sock, bool on)
{
	int v = on ? 1 : 0;
	size_t opt_len = sizeof(v);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_IMMEDIATE, &v, opt_len);
	assert(ret == 0);
}

static void set_router_mandatory(void *sock, bool on)
{
	int v = on ? 1 : 0;
	size_t opt_len = sizeof(v);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_ROUTER_MANDATORY, &v, opt_len);
	assert(ret == 0);
}

static void set_probe_router(void *sock, bool on)
{
	int v = on ? 1 : 0;
	size_t opt_len = sizeof(v);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_PROBE_ROUTER, &v, opt_len);
	assert(ret == 0);
}

static bool get_rcvmore(void *sock)
{
	int more;
	size_t opt_len = sizeof(more);
	int ret = ffi::wzmq_getsockopt(sock, ffi::WZMQ_RCVMORE, &more, &opt_len);
	assert(ret == 0);
	return more ? true : false;
}

static int get_events(void *sock)
{
	while(true)
	{
		int events;
		size_t opt_len = sizeof(events);

		int ret = ffi::wzmq_getsockopt(sock, ffi::WZMQ_EVENTS, &events, &opt_len);
		if(ret == 0)
		{
			return (int)events;
		}

		assert(errno == EINTR);
	}
}

static int get_sndhwm(void *sock)
{
	int hwm;
	size_t opt_len = sizeof(hwm);
	int ret = ffi::wzmq_getsockopt(sock, ffi::WZMQ_SNDHWM, &hwm, &opt_len);
	assert(ret == 0);
	return (int)hwm;
}

static void set_sndhwm(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_SNDHWM, &v, opt_len);
	assert(ret == 0);
}

static int get_rcvhwm(void *sock)
{
	int hwm;
	size_t opt_len = sizeof(hwm);
	int ret = ffi::wzmq_getsockopt(sock, ffi::WZMQ_RCVHWM, &hwm, &opt_len);
	assert(ret == 0);
	return (int)hwm;
}

static void set_rcvhwm(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_RCVHWM, &v, opt_len);
	assert(ret == 0);
}

static int get_hwm(void *sock)
{
	return get_sndhwm(sock);
}

static void set_hwm(void *sock, int value)
{
	set_sndhwm(sock, value);
	set_rcvhwm(sock, value);
}

static void set_tcp_keepalive(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_TCP_KEEPALIVE, &v, opt_len);
	assert(ret == 0);
}

static void set_tcp_keepalive_idle(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_TCP_KEEPALIVE_IDLE, &v, opt_len);
	assert(ret == 0);
}

static void set_tcp_keepalive_cnt(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_TCP_KEEPALIVE_CNT, &v, opt_len);
	assert(ret == 0);
}

static void set_tcp_keepalive_intvl(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = ffi::wzmq_setsockopt(sock, ffi::WZMQ_TCP_KEEPALIVE_INTVL, &v, opt_len);
	assert(ret == 0);
}

static std::mutex &g_mutex()
{
	static std::mutex m;
	return m;
}

class Global
{
public:
	Context context;
	int refs;

	Global() :
		refs(0)
	{
	}
};

static Global *global = nullptr;

static Context *addGlobalContextRef()
{
	std::lock_guard<std::mutex> guard(g_mutex());

	if(!global)
		global = new Global;

	++(global->refs);
	return &(global->context);
}

static void removeGlobalContextRef()
{
	std::lock_guard<std::mutex> guard(g_mutex());

	assert(global);
	assert(global->refs > 0);

	--(global->refs);
	if(global->refs == 0)
	{
		delete global;
		global = nullptr;
	}
}

class Socket::Private
{
public:
	Socket *q;
	bool usingGlobalContext;
	Context *context;
	void *sock;
	std::unique_ptr<SocketNotifier> sn_read;
	bool canWrite, canRead;
	std::list<CowByteArrayList> pendingWrites;
	int pendingWritten;
	std::unique_ptr<Timer> updateTimer;
	Connection updateTimerConnection;
	bool pendingUpdate;
	int shutdownWaitTime;
	bool writeQueueEnabled;

	Private(Socket *_q, Socket::Type type, Context *_context) :
		q(_q),
		canWrite(false),
		canRead(false),
		pendingWritten(0),
		pendingUpdate(false),
		shutdownWaitTime(-1),
		writeQueueEnabled(true)
	{
		if(_context)
		{
			usingGlobalContext = false;
			context = _context;
		}
		else
		{
			usingGlobalContext = true;
			context = addGlobalContextRef();
		}

		int ztype = 0;
		switch(type)
		{
			case Socket::Pair: ztype = ffi::WZMQ_PAIR; break;
			case Socket::Dealer: ztype = ffi::WZMQ_DEALER; break;
			case Socket::Router: ztype = ffi::WZMQ_ROUTER; break;
			case Socket::Req: ztype = ffi::WZMQ_REQ; break;
			case Socket::Rep: ztype = ffi::WZMQ_REP; break;
			case Socket::Push: ztype = ffi::WZMQ_PUSH; break;
			case Socket::Pull: ztype = ffi::WZMQ_PULL; break;
			case Socket::Pub: ztype = ffi::WZMQ_PUB; break;
			case Socket::Sub: ztype = ffi::WZMQ_SUB; break;
			default:
				assert(0);
		}

		sock = ffi::wzmq_socket(context->context(), ztype);
		assert(sock != NULL);

		sn_read = std::make_unique<SocketNotifier>(get_fd(sock), SocketNotifier::Read);
		sn_read->activated.connect(boost::bind(&Private::sn_read_activated, this));
		sn_read->setReadEnabled(true);

		updateTimer = std::make_unique<Timer>();
		updateTimerConnection = updateTimer->timeout.connect(boost::bind(&Private::update_timeout, this));
		updateTimer->setSingleShot(true);

		// socket notifier starts out ready. attempt to read events
		if(processEvents())
		{
			// if there are events, queue them for processing
			update();
		}
	}

	~Private()
	{
		sn_read.reset();

		set_linger(sock, shutdownWaitTime);
		ffi::wzmq_close(sock);

		if(usingGlobalContext)
			removeGlobalContextRef();
	}

	void update()
	{
		if(!pendingUpdate)
		{
			pendingUpdate = true;
			updateTimer->start();
		}
	}

	CowByteArrayList read()
	{
		if(canRead)
		{
			CowByteArrayList out;

			bool ok = true;

			do
			{
				ffi::wzmq_msg_t msg;

				int ret = ffi::wzmq_msg_init(&msg);
				assert(ret == 0);

				ret = ffi::wzmq_msg_recv(&msg, sock, ffi::WZMQ_DONTWAIT);

				if(ret < 0)
				{
					ret = ffi::wzmq_msg_close(&msg);
					assert(ret == 0);

					ok = false;
					break;
				}

				CowByteArray buf((const char *)ffi::wzmq_msg_data(&msg), ffi::wzmq_msg_size(&msg));

				ret = ffi::wzmq_msg_close(&msg);
				assert(ret == 0);

				out += buf;
			} while(get_rcvmore(sock));

			processEvents();

			if((canWrite && !pendingWrites.empty()) || canRead)
				update();

			if(ok)
				return out;
			else
				return CowByteArrayList();
		}
		else
			return CowByteArrayList();
	}

	void write(const CowByteArrayList &message)
	{
		assert(!message.isEmpty());

		if(writeQueueEnabled)
		{
			pendingWrites.push_back(message);

			if(canWrite)
				update();
		}
		else
		{
			if(zmqWrite(message))
			{
				++pendingWritten;
			}

			processEvents();

			if(pendingWritten > 0 || canRead)
				update();
		}
	}

	// return true if flags changed
	bool processEvents()
	{
		int flags = get_events(sock);

		sn_read->clearReadiness(SocketNotifier::Read);

		bool canWriteOld = canWrite;
		bool canReadOld = canRead;

		canWrite = (flags & ffi::WZMQ_POLLOUT);
		canRead = (flags & ffi::WZMQ_POLLIN);

		return (canWrite != canWriteOld || canRead != canReadOld);
	}

	bool zmqWrite(const CowByteArrayList &message)
	{
		for(int n = 0; n < message.count(); ++n)
		{
			CowByteArrayConstRef buf = message[n];

			ffi::wzmq_msg_t msg;

			int ret = ffi::wzmq_msg_init_size(&msg, buf.size());
			assert(ret == 0);

			memcpy(ffi::wzmq_msg_data(&msg), buf.data(), buf.size());

			ret = ffi::wzmq_msg_send(&msg, sock, ffi::WZMQ_DONTWAIT | (n + 1 < message.count() ? ffi::WZMQ_SNDMORE : 0));

			if(ret < 0)
			{
				ret = ffi::wzmq_msg_close(&msg);
				assert(ret == 0);

				return false;
			}

			ret = ffi::wzmq_msg_close(&msg);
			assert(ret == 0);
		}

		return true;
	}

	void tryWrite()
	{
		while(canWrite && !pendingWrites.empty())
		{
			// whether this write succeeds or not, we assume we
			//   can't write afterwards
			canWrite = false;

			if(zmqWrite(pendingWrites.front()))
			{
				pendingWrites.pop_front();
				++pendingWritten;
			}

			processEvents();
		}
	}

	void doUpdate()
	{
		tryWrite();

		if(canRead)
		{
			std::weak_ptr<Private> self = q->d;
			q->readyRead();
			if(self.expired())
				return;
		}

		if(pendingWritten > 0)
		{
			int count = pendingWritten;
			pendingWritten = 0;

			q->messagesWritten(count);
		}
	}

	void update_timeout()
	{
		pendingUpdate = false;

		doUpdate();
	}

	void sn_read_activated()
	{
		if(!processEvents())
			return;

		if(pendingUpdate)
		{
			pendingUpdate = false;
			updateTimer->stop();
		}

		doUpdate();
	}
};

Socket::Socket(Type type)
{
	d = std::make_shared<Private>(this, type, nullptr);
}

Socket::Socket(Type type, Context *context)
{
	d = std::make_shared<Private>(this, type, context);
}

Socket::~Socket() = default;

void Socket::setShutdownWaitTime(int msecs)
{
	d->shutdownWaitTime = msecs;
}

void Socket::setWriteQueueEnabled(bool enable)
{
	d->writeQueueEnabled = enable;
}

void Socket::subscribe(const CowByteArray &filter)
{
	set_subscribe(d->sock, filter.data(), filter.size());
}

void Socket::unsubscribe(const CowByteArray &filter)
{
	set_unsubscribe(d->sock, filter.data(), filter.size());
}

CowByteArray Socket::identity() const
{
	CowByteArray buf(255, 0);
	buf.resize(get_identity(d->sock, buf.data(), buf.size()));
	return buf;
}

void Socket::setIdentity(const CowByteArray &id)
{
	set_identity(d->sock, id.data(), id.size());
}

int Socket::hwm() const
{
	return get_hwm(d->sock);
}

void Socket::setHwm(int hwm)
{
	set_hwm(d->sock, hwm);
}

int Socket::sendHwm() const
{
	return get_sndhwm(d->sock);
}

int Socket::receiveHwm() const
{
	return get_rcvhwm(d->sock);
}

void Socket::setSendHwm(int hwm)
{
	set_sndhwm(d->sock, hwm);
}

void Socket::setReceiveHwm(int hwm)
{
	set_rcvhwm(d->sock, hwm);
}

void Socket::setImmediateEnabled(bool on)
{
	set_immediate(d->sock, on);
}

void Socket::setRouterMandatoryEnabled(bool on)
{
	set_router_mandatory(d->sock, on);
}

void Socket::setProbeRouterEnabled(bool on)
{
	set_probe_router(d->sock, on);
}

void Socket::setTcpKeepAliveEnabled(bool on)
{
	set_tcp_keepalive(d->sock, on ? 1 : 0);
}

void Socket::setTcpKeepAliveParameters(int idle, int count, int interval)
{
	set_tcp_keepalive_idle(d->sock, idle);
	set_tcp_keepalive_cnt(d->sock, count);
	set_tcp_keepalive_intvl(d->sock, interval);
}

void Socket::connectToAddress(const CowString &addr)
{
	int ret = ffi::wzmq_connect(d->sock, addr.toUtf8().data());
	assert(ret == 0);
}

bool Socket::bind(const CowString &addr)
{
	int ret = ffi::wzmq_bind(d->sock, addr.toUtf8().data());
	if(ret != 0)
		return false;

	return true;
}

bool Socket::canRead() const
{
	return d->canRead;
}

bool Socket::canWriteImmediately() const
{
	return d->canWrite;
}

CowByteArrayList Socket::read()
{
	return d->read();
}

void Socket::write(const CowByteArrayList &message)
{
	d->write(message);
}

}
