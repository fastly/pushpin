/*
 * Copyright (C) 2012-2020 Justin Karneges
 * Copyright (C) 2025 Fastly, Inc.
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

#include "qzmqvalve.h"

#include "qzmqsocket.h"
#include "defercall.h"

namespace QZmq {

class Valve::Private
{
public:
	Valve *q;
	QZmq::Socket *sock;
	bool isOpen;
	bool pendingRead;
	int maxReadsPerEvent;
	boost::signals2::scoped_connection rrConnection;
	DeferCall deferCall;

	Private(Valve *_q) :
		q(_q),
		sock(0),
		isOpen(false),
		pendingRead(false),
		maxReadsPerEvent(100)
	{
	}

	void setup(QZmq::Socket *_sock)
	{
		sock = _sock;
		rrConnection = sock->readyRead.connect(boost::bind(&Private::sock_readyRead, this));
	}

	void queueRead()
	{
		if(pendingRead)
			return;

		pendingRead = true;
		deferCall.defer([=] { queuedRead(); });
	}

	void tryRead()
	{
		std::weak_ptr<Private> self = q->d;

		int count = 0;
		while(isOpen && sock->canRead())
		{
			if(count >= maxReadsPerEvent)
			{
				queueRead();
				return;
			}

			QList<QByteArray> msg = sock->read();

			if(!msg.isEmpty())
			{
				q->readyRead(msg);
				if(self.expired())
					return;
			}

			++count;
		}
	}

	void sock_readyRead()
	{
		if(pendingRead)
			return;

		tryRead();
	}

	void queuedRead()
	{
		pendingRead = false;
		tryRead();
	}
};

Valve::Valve(QZmq::Socket *sock, QObject *parent) :
	QObject(parent)
{
	d = std::make_shared<Private>(this);
	d->setup(sock);
}

Valve::~Valve() = default;

bool Valve::isOpen() const
{
	return d->isOpen;
}

void Valve::setMaxReadsPerEvent(int max)
{
	d->maxReadsPerEvent = max;
}

void Valve::open()
{
	if(!d->isOpen)
	{
		d->isOpen = true;
		if(!d->pendingRead && d->sock->canRead())
			d->queueRead();
	}
}

void Valve::close()
{
	d->isOpen = false;
}

}
