/*
 * Copyright (C) 2012-2020 Justin Karneges
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

#include <stdio.h>
#include <assert.h>
#include <QStringList>
#include <QPointer>
#include <QTimer>
#include <QSocketNotifier>
#include <QMutex>
#include <zmq.h>
#include "qzmqcontext.h"

namespace QZmq {

static int get_fd(void *sock)
{
	int fd;
	size_t opt_len = sizeof(fd);
	int ret = zmq_getsockopt(sock, ZMQ_FD, &fd, &opt_len);
	assert(ret == 0);
	return fd;
}

static void set_subscribe(void *sock, const char *data, int size)
{
	size_t opt_len = size;
	int ret = zmq_setsockopt(sock, ZMQ_SUBSCRIBE, data, opt_len);
	assert(ret == 0);
}

static void set_unsubscribe(void *sock, const char *data, int size)
{
	size_t opt_len = size;
	zmq_setsockopt(sock, ZMQ_UNSUBSCRIBE, data, opt_len);
	// note: we ignore errors, such as unsubscribing a nonexisting filter
}

static void set_linger(void *sock, int value)
{
	size_t opt_len = sizeof(value);
	int ret = zmq_setsockopt(sock, ZMQ_LINGER, &value, opt_len);
	assert(ret == 0);
}

static int get_identity(void *sock, char *data, int size)
{
	size_t opt_len = size;
	int ret = zmq_getsockopt(sock, ZMQ_IDENTITY, data, &opt_len);
	assert(ret == 0);
	return (int)opt_len;
}

static void set_identity(void *sock, const char *data, int size)
{
	size_t opt_len = size;
	int ret = zmq_setsockopt(sock, ZMQ_IDENTITY, data, opt_len);
	if(ret != 0)
		printf("%d\n", errno);
	assert(ret == 0);
}

#if ZMQ_VERSION_MAJOR >= 4

static void set_immediate(void *sock, bool on)
{
	int v = on ? 1 : 0;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_IMMEDIATE, &v, opt_len);
	assert(ret == 0);
}

#else

static void set_immediate(void *sock, bool on)
{
	int v = on ? 1 : 0;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_DELAY_ATTACH_ON_CONNECT, &v, opt_len);
	assert(ret == 0);
}

#endif

#if (ZMQ_VERSION_MAJOR >= 4) || ((ZMQ_VERSION_MAJOR >= 3) && (ZMQ_VERSION_MINOR >= 2))

#define USE_MSG_IO

static bool get_rcvmore(void *sock)
{
	int more;
	size_t opt_len = sizeof(more);
	int ret = zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &opt_len);
	assert(ret == 0);
	return more ? true : false;
}

static int get_events(void *sock)
{
	while(true)
	{
		int events;
		size_t opt_len = sizeof(events);

		int ret = zmq_getsockopt(sock, ZMQ_EVENTS, &events, &opt_len);
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
	int ret = zmq_getsockopt(sock, ZMQ_SNDHWM, &hwm, &opt_len);
	assert(ret == 0);
	return (int)hwm;
}

static void set_sndhwm(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_SNDHWM, &v, opt_len);
	assert(ret == 0);
}

static int get_rcvhwm(void *sock)
{
	int hwm;
	size_t opt_len = sizeof(hwm);
	int ret = zmq_getsockopt(sock, ZMQ_RCVHWM, &hwm, &opt_len);
	assert(ret == 0);
	return (int)hwm;
}

static void set_rcvhwm(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_RCVHWM, &v, opt_len);
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
	int ret = zmq_setsockopt(sock, ZMQ_TCP_KEEPALIVE, &v, opt_len);
	assert(ret == 0);
}

static void set_tcp_keepalive_idle(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_TCP_KEEPALIVE_IDLE, &v, opt_len);
	assert(ret == 0);
}

static void set_tcp_keepalive_cnt(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_TCP_KEEPALIVE_CNT, &v, opt_len);
	assert(ret == 0);
}

static void set_tcp_keepalive_intvl(void *sock, int value)
{
	int v = value;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_TCP_KEEPALIVE_INTVL, &v, opt_len);
	assert(ret == 0);
}

#else

static bool get_rcvmore(void *sock)
{
	qint64 more;
	size_t opt_len = sizeof(more);
	int ret = zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &opt_len);
	assert(ret == 0);
	return more ? true : false;
}

static int get_events(void *sock)
{
	while(true)
	{
		quint32 events;
		size_t opt_len = sizeof(events);

		int ret = zmq_getsockopt(sock, ZMQ_EVENTS, &events, &opt_len);
		if(ret == 0)
		{
			return (int)events;
		}

		assert(errno == EINTR);
	}
}

static int get_hwm(void *sock)
{
	quint64 hwm;
	size_t opt_len = sizeof(hwm);
	int ret = zmq_getsockopt(sock, ZMQ_HWM, &hwm, &opt_len);
	assert(ret == 0);
	return (int)hwm;
}

static void set_hwm(void *sock, int value)
{
	quint64 v = value;
	size_t opt_len = sizeof(v);
	int ret = zmq_setsockopt(sock, ZMQ_HWM, &v, opt_len);
	assert(ret == 0);
}

static int get_sndhwm(void *sock)
{
	return get_hwm(sock);
}

static void set_sndhwm(void *sock, int value)
{
	set_hwm(sock, value);
}

static int get_rcvhwm(void *sock)
{
	return get_hwm(sock);
}

static void set_rcvhwm(void *sock, int value)
{
	set_hwm(sock, value);
}

static void set_tcp_keepalive(void *sock, int value)
{
	// not supported for this zmq version
	Q_UNUSED(sock);
	Q_UNUSED(on);
}

static void set_tcp_keepalive_idle(void *sock, int value)
{
	// not supported for this zmq version
	Q_UNUSED(sock);
	Q_UNUSED(on);
}

static void set_tcp_keepalive_cnt(void *sock, int value)
{
	// not supported for this zmq version
	Q_UNUSED(sock);
	Q_UNUSED(on);
}

static void set_tcp_keepalive_intvl(void *sock, int value)
{
	// not supported for this zmq version
	Q_UNUSED(sock);
	Q_UNUSED(on);
}

#endif

Q_GLOBAL_STATIC(QMutex, g_mutex)

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

static Global *global = 0;

static Context *addGlobalContextRef()
{
	QMutexLocker locker(g_mutex());

	if(!global)
		global = new Global;

	++(global->refs);
	return &(global->context);
}

static void removeGlobalContextRef()
{
	QMutexLocker locker(g_mutex());

	assert(global);
	assert(global->refs > 0);

	--(global->refs);
	if(global->refs == 0)
	{
		delete global;
		global = 0;
	}
}

class Socket::Private : public QObject
{
	Q_OBJECT

public:
	Socket *q;
	bool usingGlobalContext;
	Context *context;
	void *sock;
	QSocketNotifier *sn_read;
	bool canWrite, canRead;
	QList< QList<QByteArray> > pendingWrites;
	int pendingWritten;
	QTimer *updateTimer;
	bool pendingUpdate;
	int shutdownWaitTime;
	bool writeQueueEnabled;

	Private(Socket *_q, Socket::Type type, Context *_context) :
		QObject(_q),
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
			case Socket::Pair: ztype = ZMQ_PAIR; break;
			case Socket::Dealer: ztype = ZMQ_DEALER; break;
			case Socket::Router: ztype = ZMQ_ROUTER; break;
			case Socket::Req: ztype = ZMQ_REQ; break;
			case Socket::Rep: ztype = ZMQ_REP; break;
			case Socket::Push: ztype = ZMQ_PUSH; break;
			case Socket::Pull: ztype = ZMQ_PULL; break;
			case Socket::Pub: ztype = ZMQ_PUB; break;
			case Socket::Sub: ztype = ZMQ_SUB; break;
			default:
				assert(0);
		}

		sock = zmq_socket(context->context(), ztype);
		assert(sock != NULL);

		sn_read = new QSocketNotifier(get_fd(sock), QSocketNotifier::Read, this);
		connect(sn_read, SIGNAL(activated(int)), SLOT(sn_read_activated()));
		sn_read->setEnabled(true);

		updateTimer = new QTimer(this);
		connect(updateTimer, SIGNAL(timeout()), SLOT(update_timeout()));
		updateTimer->setSingleShot(true);
	}

	~Private()
	{
		updateTimer->disconnect(this);
		updateTimer->setParent(0);
		updateTimer->deleteLater();

		set_linger(sock, shutdownWaitTime);
		zmq_close(sock);

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

	QList<QByteArray> read()
	{
		if(canRead)
		{
			QList<QByteArray> out;

			bool ok = true;

			do
			{
				zmq_msg_t msg;

				int ret = zmq_msg_init(&msg);
				assert(ret == 0);

#ifdef USE_MSG_IO
				ret = zmq_msg_recv(&msg, sock, ZMQ_DONTWAIT);
#else
				ret = zmq_recv(sock, &msg, ZMQ_NOBLOCK);
#endif

				if(ret < 0)
				{
					ret = zmq_msg_close(&msg);
					assert(ret == 0);

					ok = false;
					break;
				}

				QByteArray buf((const char *)zmq_msg_data(&msg), zmq_msg_size(&msg));

				ret = zmq_msg_close(&msg);
				assert(ret == 0);

				out += buf;
			} while(get_rcvmore(sock));

			processEvents();

			if((canWrite && !pendingWrites.isEmpty()) || canRead)
				update();

			if(ok)
				return out;
			else
				return QList<QByteArray>();
		}
		else
			return QList<QByteArray>();
	}

	void write(const QList<QByteArray> &message)
	{
		assert(!message.isEmpty());

		if(writeQueueEnabled)
		{
			pendingWrites += message;

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

		bool canWriteOld = canWrite;
		bool canReadOld = canRead;

		canWrite = (flags & ZMQ_POLLOUT);
		canRead = (flags & ZMQ_POLLIN);

		return (canWrite != canWriteOld || canRead != canReadOld);
	}

	bool zmqWrite(const QList<QByteArray> &message)
	{
		for(int n = 0; n < message.count(); ++n)
		{
			const QByteArray &buf = message[n];

			zmq_msg_t msg;

			int ret = zmq_msg_init_size(&msg, buf.size());
			assert(ret == 0);

			memcpy(zmq_msg_data(&msg), buf.data(), buf.size());

#ifdef USE_MSG_IO
			ret = zmq_msg_send(&msg, sock, ZMQ_DONTWAIT | (n + 1 < message.count() ? ZMQ_SNDMORE : 0));
#else
			ret = zmq_send(sock, &msg, ZMQ_NOBLOCK | (n + 1 < message.count() ? ZMQ_SNDMORE : 0));
#endif

			if(ret < 0)
			{
				ret = zmq_msg_close(&msg);
				assert(ret == 0);

				return false;
			}

			ret = zmq_msg_close(&msg);
			assert(ret == 0);
		}

		return true;
	}

	void tryWrite()
	{
		while(canWrite && !pendingWrites.isEmpty())
		{
			// whether this write succeeds or not, we assume we
			//   can't write afterwards
			canWrite = false;

			if(zmqWrite(pendingWrites.first()))
			{
				pendingWrites.removeFirst();
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
			QPointer<QObject> self = this;
			emit q->readyRead();
			if(!self)
				return;
		}

		if(pendingWritten > 0)
		{
			int count = pendingWritten;
			pendingWritten = 0;

			emit q->messagesWritten(count);
		}
	}

public slots:
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

	void update_timeout()
	{
		pendingUpdate = false;

		doUpdate();
	}
};

Socket::Socket(Type type, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, type, 0);
}

Socket::Socket(Type type, Context *context, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, type, context);
}

Socket::~Socket()
{
	delete d;
}

void Socket::setShutdownWaitTime(int msecs)
{
	d->shutdownWaitTime = msecs;
}

void Socket::setWriteQueueEnabled(bool enable)
{
	d->writeQueueEnabled = enable;
}

void Socket::subscribe(const QByteArray &filter)
{
	set_subscribe(d->sock, filter.data(), filter.size());
}

void Socket::unsubscribe(const QByteArray &filter)
{
	set_unsubscribe(d->sock, filter.data(), filter.size());
}

QByteArray Socket::identity() const
{
	QByteArray buf(255, 0);
	buf.resize(get_identity(d->sock, buf.data(), buf.size()));
	return buf;
}

void Socket::setIdentity(const QByteArray &id)
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

void Socket::connectToAddress(const QString &addr)
{
	int ret = zmq_connect(d->sock, addr.toUtf8().data());
	assert(ret == 0);
}

bool Socket::bind(const QString &addr)
{
	int ret = zmq_bind(d->sock, addr.toUtf8().data());
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

QList<QByteArray> Socket::read()
{
	return d->read();
}

void Socket::write(const QList<QByteArray> &message)
{
	d->write(message);
}

}

#include "qzmqsocket.moc"
