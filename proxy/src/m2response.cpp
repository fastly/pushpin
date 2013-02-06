/*
 * Copyright (C) 2012-2013 Fan Out Networks, Inc.
 *
 * This file is part of Pushpin.
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "m2response.h"

#include <QPointer>
#include <QTimer>
#include "packet/m2responsepacket.h"
#include "log.h"
#include "m2manager.h"

// 100k/second
#define RATE_TIME 1000
#define RATE_SIZE 100000

static QByteArray makeChunk(const QByteArray &in)
{
	QByteArray out;
	out += QByteArray::number(in.size(), 16).toUpper() + "\r\n";
	out += in;
	out += "\r\n";
	return out;
}

class M2Response::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Stopped,
		Starting,
		SendingBody
	};

	M2Response *q;
	M2Manager *manager;
	M2Request::Rid rid;
	State state;
	bool pendingUpdate;
	int code;
	QByteArray status;
	HttpHeaders headers;
	QByteArray out;
	bool outFinished;
	bool chunked;
	QTimer *timer;
	int outCredits;

	Private(M2Response *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		state(Stopped),
		pendingUpdate(false),
		outFinished(false),
		chunked(false),
		outCredits(0)
	{
		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(timer)
		{
			timer->disconnect(this);
			timer->setParent(0);
			timer->deleteLater();
			timer = 0;
		}
	}

	void update()
	{
		if(!pendingUpdate)
		{
			pendingUpdate = true;
			QMetaObject::invokeMethod(this, "doUpdate", Qt::QueuedConnection);
		}
	}

	void writeBodyResponse(const QByteArray &body)
	{
		M2ResponsePacket p;
		p.sender = rid.first;
		p.id = rid.second;
		if(chunked)
			p.data = makeChunk(body);
		else
			p.data = body;

		manager->writeResponse(p);
	}

	// for chunked mode, this will write a final chunk but leave
	//   the connection alone. for non-chunked, this will instruct
	//   mongrel2 to close the HTTP connection, which some clients
	//   seem to need
	void writeCloseResponse()
	{
		writeBodyResponse("");
	}

public slots:
	void doUpdate()
	{
		QPointer<QObject> self = this;

		pendingUpdate = false;

		if(state == Starting)
		{
			timer->start(RATE_TIME);

			if(headers.get("Transfer-Encoding") == "chunked")
				chunked = true;

			M2ResponsePacket p;

			p.sender = rid.first;
			p.id = rid.second;
			p.data = "HTTP/1.1 " + QByteArray::number(code) + ' ' + status + "\r\n";
			foreach(const HttpHeader &h, headers)
				p.data += h.first + ": " + h.second + "\r\n";
			p.data += "\r\n";

			outCredits = RATE_SIZE;

			int bodySize = qMin(out.size(), outCredits);

			if(bodySize > 0)
			{
				QByteArray buf = out.mid(0, bodySize);
				out = out.mid(bodySize);

				if(chunked)
					p.data += makeChunk(buf);
				else
					p.data += buf;

				outCredits -= bodySize;
			}

			manager->writeResponse(p);

			if(bodySize > 0)
			{
				emit q->bytesWritten(bodySize);
				if(!self)
					return;
			}

			if(outFinished)
			{
				writeCloseResponse();

				state = Stopped;
				emit q->finished();
			}
			else
				state = SendingBody;
		}
		else if(state == SendingBody)
		{
			if(!out.isEmpty())
			{
				if(!timer->isActive())
					timer->start(RATE_TIME);

				int bodySize = qMin(out.size(), outCredits);

				if(bodySize > 0)
				{
					QByteArray buf = out.mid(0, bodySize);
					out = out.mid(bodySize);

					outCredits -= bodySize;
					writeBodyResponse(buf);

					emit q->bytesWritten(bodySize);
					if(!self)
						return;
				}
			}

			if(out.isEmpty() && outFinished)
			{
				writeCloseResponse();

				state = Stopped;
				emit q->finished();
			}
		}
	}

	void timer_timeout()
	{
		outCredits = RATE_SIZE;

		if(!out.isEmpty())
			doUpdate();
		else
			timer->stop();
	}
};

M2Response::M2Response(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

M2Response::~M2Response()
{
	delete d;
}

void M2Response::start(int code, const QByteArray &status, const HttpHeaders &headers)
{
	d->state = Private::Starting;
	d->code = code;
	d->status = status;
	d->headers = headers;

	d->update();
}

void M2Response::write(const QByteArray &body)
{
	d->out += body;

	d->update();
}

void M2Response::close()
{
	d->outFinished = true;

	d->update();
}

void M2Response::handle(M2Manager *manager, const M2Request::Rid &rid)
{
	d->manager = manager;
	d->rid = rid;
}

#include "m2response.moc"
