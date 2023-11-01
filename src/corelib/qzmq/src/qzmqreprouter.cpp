/*
 * Copyright (C) 2012 Justin Karneges
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

#include "qzmqreprouter.h"

#include "qzmqsocket.h"
#include "qzmqreqmessage.h"

namespace QZmq {

class RepRouter::Private : public QObject
{
	Q_OBJECT

public:
	RepRouter *q;
	Socket *sock;

	Private(RepRouter *_q) :
		QObject(_q),
		q(_q)
	{
		sock = new Socket(Socket::Router, this);
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(sock, SIGNAL(messagesWritten(int)), SLOT(sock_messagesWritten(int)));
	}

public slots:
	void sock_readyRead()
	{
		emit q->readyRead();
	}

	void sock_messagesWritten(int count)
	{
		emit q->messagesWritten(count);
	}
};

RepRouter::RepRouter(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

RepRouter::~RepRouter()
{
	delete d;
}

void RepRouter::setShutdownWaitTime(int msecs)
{
	d->sock->setShutdownWaitTime(msecs);
}

void RepRouter::connectToAddress(const QString &addr)
{
	d->sock->connectToAddress(addr);
}

bool RepRouter::bind(const QString &addr)
{
	return d->sock->bind(addr);
}

bool RepRouter::canRead() const
{
	return d->sock->canRead();
}

ReqMessage RepRouter::read()
{
	return ReqMessage(d->sock->read());
}

void RepRouter::write(const ReqMessage &message)
{
	d->sock->write(message.toRawMessage());
}

}

#include "qzmqreprouter.moc"
