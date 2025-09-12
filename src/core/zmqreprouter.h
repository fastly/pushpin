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

#ifndef ZMQREPROUTER_H
#define ZMQREPROUTER_H

#include <boost/signals2.hpp>

class CowString;

using Signal = boost::signals2::signal<void()>;
using SignalInt = boost::signals2::signal<void(int)>;
using Connection = boost::signals2::scoped_connection;

class ZmqReqMessage;

class ZmqRepRouter
{
public:
	ZmqRepRouter();
	~ZmqRepRouter();

	void setShutdownWaitTime(int msecs);

	void connectToAddress(const CowString &addr);
	bool bind(const CowString &addr);

	bool canRead() const;

	ZmqReqMessage read();
	void write(const ZmqReqMessage &message);

	Signal readyRead;
	SignalInt messagesWritten;

private:
	ZmqRepRouter(const ZmqRepRouter &) = delete;
	ZmqRepRouter &operator=(const ZmqRepRouter &) = delete;

	class Private;
	friend class Private;
	std::unique_ptr<Private> d;
};

#endif
