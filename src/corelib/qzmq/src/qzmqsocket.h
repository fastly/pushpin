/*
 * Copyright (C) 2012-2015 Justin Karneges
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

#ifndef QZMQSOCKET_H
#define QZMQSOCKET_H

#include <QObject>

namespace QZmq {

class Context;

class Socket : public QObject
{
	Q_OBJECT

public:
	enum Type
	{
		Pair,
		Dealer,
		Router,
		Req,
		Rep,
		Push,
		Pull,
		Pub,
		Sub
	};

	Socket(Type type, QObject *parent = 0);
	Socket(Type type, Context *context, QObject *parent = 0);
	~Socket();

	// 0 means drop queue and don't block, -1 means infinite (default = -1)
	void setShutdownWaitTime(int msecs);

	// if enabled, messages are queued internally until the socket is able
	//   to accept them. the messagesWritten signal is emitted once writes
	//   have succeeded. otherwise, messages are passed directly to
	//   zmq_send and dropped if they can't be written. default enabled.
	// disabling the queue is good for socket types where the HWM has a
	//   drop policy. enabling the queue is good when the HWM has a
	//   blocking policy.
	void setWriteQueueEnabled(bool enable);

	void subscribe(const QByteArray &filter);
	void unsubscribe(const QByteArray &filter);

	QByteArray identity() const;
	void setIdentity(const QByteArray &id);

	// deprecated, zmq 2.x
	int hwm() const;
	void setHwm(int hwm);

	int sendHwm() const;
	int receiveHwm() const;
	void setSendHwm(int hwm);
	void setReceiveHwm(int hwm);

	void setImmediateEnabled(bool on);

	void setTcpKeepAliveEnabled(bool on);
	void setTcpKeepAliveParameters(int idle = -1, int count = -1, int interval = -1);

	void connectToAddress(const QString &addr);
	bool bind(const QString &addr);

	bool canRead() const;

	// returns true if this object believes the next write to zmq will
	//   succeed immediately. note that it starts out false until the
	//   value is discovered. also note that the write could still end up
	//   needing to be queued, if the conditions change in between.
	bool canWriteImmediately() const;

	QList<QByteArray> read();
	void write(const QList<QByteArray> &message);

signals:
	void readyRead();
	void messagesWritten(int count);

private:
	Q_DISABLE_COPY(Socket)

	class Private;
	friend class Private;
	Private *d;
};

}

#endif
