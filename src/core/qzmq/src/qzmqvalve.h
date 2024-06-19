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

#ifndef QZMQVALVE_H
#define QZMQVALVE_H

#include <QObject>
#include <boost/signals2.hpp>

using SignalList = boost::signals2::signal<void(const QList<QByteArray>&)>;
using Connection = boost::signals2::scoped_connection;

namespace QZmq {

class Socket;

class Valve : public QObject
{
	Q_OBJECT

public:
	Valve(QZmq::Socket *sock, QObject *parent = 0);
	~Valve();

	bool isOpen() const;

	void setMaxReadsPerEvent(int max);

	void open();
	void close();

	SignalList readyRead;

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
