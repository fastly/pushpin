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

#include "zmqreprouter.h"

#include "cowstring.h"
#include "zmqsocket.h"
#include "zmqreqmessage.h"

class ZmqRepRouter::Private
{
public:
	ZmqRepRouter *q;
	std::unique_ptr<ZmqSocket> sock;
	Connection mWConnection;
	Connection rrConnection;

	Private(ZmqRepRouter *_q) :
		q(_q)
	{
		sock = std::make_unique<ZmqSocket>(ZmqSocket::Router);
		rrConnection = sock->readyRead.connect(boost::bind(&Private::sock_readyRead, this));
		mWConnection = sock->messagesWritten.connect(boost::bind(&Private::sock_messagesWritten, this,  boost::placeholders::_1));
	}

	void sock_messagesWritten(int count)
	{
		q->messagesWritten(count);
	}

	void sock_readyRead()
	{
		q->readyRead();
	}
};

ZmqRepRouter::ZmqRepRouter()
{
	d = std::make_unique<Private>(this);
}

ZmqRepRouter::~ZmqRepRouter() = default;

void ZmqRepRouter::setShutdownWaitTime(int msecs)
{
	d->sock->setShutdownWaitTime(msecs);
}

void ZmqRepRouter::connectToAddress(const CowString &addr)
{
	d->sock->connectToAddress(addr);
}

bool ZmqRepRouter::bind(const CowString &addr)
{
	return d->sock->bind(addr);
}

bool ZmqRepRouter::canRead() const
{
	return d->sock->canRead();
}

ZmqReqMessage ZmqRepRouter::read()
{
	return ZmqReqMessage(d->sock->read());
}

void ZmqRepRouter::write(const ZmqReqMessage &message)
{
	d->sock->write(message.toRawMessage());
}
