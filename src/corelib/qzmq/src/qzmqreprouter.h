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

#ifndef QZMQREPROUTER_H
#define QZMQREPROUTER_H

#include <QObject>

namespace QZmq {

class ReqMessage;

class RepRouter : public QObject
{
	Q_OBJECT

public:
	RepRouter(QObject *parent = 0);
	~RepRouter();

	void setShutdownWaitTime(int msecs);

	void connectToAddress(const QString &addr);
	bool bind(const QString &addr);

	bool canRead() const;

	ReqMessage read();
	void write(const ReqMessage &message);

signals:
	void readyRead();
	void messagesWritten(int count);

private:
	Q_DISABLE_COPY(RepRouter)

	class Private;
	friend class Private;
	Private *d;
};

}

#endif
