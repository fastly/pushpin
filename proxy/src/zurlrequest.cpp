/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
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

#include "zurlrequest.h"

#include <assert.h>
#include <QTimer>
#include <QPointer>
#include <QUuid>
#include <QUrl>
#include "packet/zurlrequestpacket.h"
#include "packet/zurlresponsepacket.h"
#include "log.h"
#include "zurlmanager.h"

#define IDEAL_CREDITS 200000
#define REQUEST_TIMEOUT 600

class ZurlRequest::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Stopped,           // response finished, error, or not even started
		Starting,          // we're prepared to send the first packet
		RequestStartWait,  // we've sent the first packet of streamed input, waiting for ack
		Requesting,        // we're sending the rest of streamed input
		RequestFinishWait, // we've completed sending the request, waiting for ack
		Receiving          // we've completed sending the request, waiting on response
	};

	ZurlRequest *q;
	ZurlManager *manager;
	State state;
	ZurlRequest::Rid rid;
	QByteArray replyAddress;
	QString connectHost;
	QString method;
	QUrl url;
	HttpHeaders headers;
	QByteArray out;
	int inSeq;
	int outSeq;
	int outCredits;
	bool outFinished; // user has finished providng input
	int pendingInCredits;
	bool haveResponseValues;
	int responseCode;
	QByteArray responseStatus;
	HttpHeaders responseHeaders;
	QByteArray in;
	bool pendingUpdate;
	ZurlRequest::ErrorCondition errorCondition;
	QTimer *timer;

	Private(ZurlRequest *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		state(Stopped),
		inSeq(0),
		outSeq(0),
		outCredits(0),
		outFinished(false),
		pendingInCredits(0),
		haveResponseValues(false),
		pendingUpdate(false),
		timer(0)
	{
	}

	~Private()
	{
		if(manager)
		{
			// if we're in the middle of stuff, and have received an ack from the server,
			//   then cancel the request
			if(state == Requesting || state == Receiving)
			{
				ZurlRequestPacket p;
				p.id = rid.second;
				p.seq = outSeq++;
				p.cancel = true;
				manager->write(p, replyAddress);
			}
		}

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

		if(manager)
		{
			manager->unlink(q);
			manager = 0;
		}
	}

	void refreshTimeout()
	{
		timer->start(REQUEST_TIMEOUT * 1000);
	}

	void update()
	{
		if(!pendingUpdate)
		{
			pendingUpdate = true;
			QMetaObject::invokeMethod(this, "doUpdate", Qt::QueuedConnection);
		}
	}

	QByteArray readResponseBody(int size)
	{
		if(size != -1)
			size = qMin(size, in.size());
		else
			size = in.size();

		if(size == 0)
			return QByteArray();

		QByteArray out = in.mid(0, size);
		in = in.mid(size);

		pendingInCredits += size;

		if(state == Receiving)
			tryWrite(); // this should not emit signals in current state

		return out;
	}

	void tryWrite()
	{
		if(state == Requesting)
		{
			// if all we have to send is EOF, we don't need credits for that
			if(out.isEmpty() && outFinished)
			{
				ZurlRequestPacket p;
				p.id = rid.second;
				p.seq = outSeq++;
				p.body = QByteArray(""); // need to set body to count as content packet

				state = Receiving;
				manager->write(p, replyAddress);
				refreshTimeout();
				return;
			}

			if(!out.isEmpty() && outCredits > 0)
			{
				// if we have data to send, and the credits to do so, then send data.
				// also send credits if we need to.

				int size = qMin(outCredits, out.size());

				QByteArray buf = out.mid(0, size);
				out = out.mid(size);

				outCredits -= size;

				ZurlRequestPacket p;
				p.id = rid.second;
				p.seq = outSeq++;
				p.body = buf;
				if(!out.isEmpty() || !outFinished)
					p.more = true;
				if(pendingInCredits > 0)
				{
					p.credits = pendingInCredits;
					pendingInCredits = 0;
				}

				if(!p.more)
					state = Receiving;

				manager->write(p, replyAddress);
				refreshTimeout();
				emit q->bytesWritten(size);
			}
		}
		else if(state == Receiving)
		{
			if(pendingInCredits > 0)
			{
				// if we have no data to send but we need to send credits, do at least that
				ZurlRequestPacket p;
				p.id = rid.second;
				p.seq = outSeq++;
				p.credits = pendingInCredits;
				pendingInCredits = 0;

				manager->write(p, replyAddress);
				refreshTimeout();
			}
		}
	}

	void handle(const ZurlResponsePacket &packet)
	{
		if(state == RequestStartWait)
		{
			if(packet.replyAddress.isEmpty())
			{
				state = Private::Stopped;
				errorCondition = ErrorGeneric;
				cleanup();
				log_warning("initial ack for streamed input request did not contain reply-address");
				emit q->error();
				return;
			}

			replyAddress = packet.replyAddress;

			state = Requesting;
		}
		else if(state == RequestFinishWait)
		{
			replyAddress = packet.replyAddress;

			state = Receiving;
		}

		if(packet.isError)
		{
			// zurl conditions:
			//  remote-connection-failed
			//  connection-timeout
			//  tls-error
			//  bad-request
			//  policy-violation
			//  max-size-exceeded
			//  cancel

			QByteArray cond = packet.condition;
			if(cond == "policy-violation")
				errorCondition = ErrorPolicy;
			else if(cond == "remote-connection-failed")
				errorCondition = ErrorConnect;
			else if(cond == "tls-error")
				errorCondition = ErrorTls;
			else if(cond == "length-required")
				errorCondition = ErrorLengthRequired;
			else if(cond == "connection-timeout")
				errorCondition = ErrorTimeout;
			else // bad-request, max-size-exceeded, cancel
				errorCondition = ErrorGeneric;

			state = Private::Stopped;
			cleanup();
			emit q->error();
			return;
		}

		if(packet.seq != inSeq)
		{
			log_warning("received message out of sequence, canceling");

			// if this was not an error packet, send cancel
			if(packet.condition.isEmpty())
			{
				ZurlRequestPacket p;
				p.id = rid.second;
				p.seq = outSeq++;
				p.cancel = true;
				manager->write(p, replyAddress);
			}

			state = Private::Stopped;
			errorCondition = ErrorGeneric;
			cleanup();
			emit q->error();
			return;
		}

		++inSeq;

		refreshTimeout();

		bool doReadyRead = false;

		if(!packet.body.isNull())
		{
			if(!haveResponseValues)
			{
				haveResponseValues = true;

				responseCode = packet.code;
				responseStatus = packet.status;
				responseHeaders = packet.headers;
			}

			if(in.size() + packet.body.size() > IDEAL_CREDITS)
				log_warning("zurl is sending too fast");

			in += packet.body;

			if(packet.more)
			{
				if(!packet.body.isEmpty())
					doReadyRead = true;
			}
			else
			{
				// always emit readyRead here even if body is empty, for EOF
				state = Private::Stopped;
				cleanup();
				emit q->readyRead();
				return;
			}
		}

		if(packet.credits > 0)
			outCredits += packet.credits;

		// the only reason we should need to write as a result of a read
		//   is if we were given credits or we have credits to give
		if(packet.credits > 0 || pendingInCredits > 0)
		{
			QPointer<QObject> self = this;
			tryWrite();
			if(!self)
				return;
		}

		if(doReadyRead)
			emit q->readyRead();
	}

public slots:
	void doUpdate()
	{
		pendingUpdate = false;

		if(state == Starting)
		{
			if(!manager->canWriteImmediately())
			{
				state = Private::Stopped;
				errorCondition = ZurlRequest::ErrorUnavailable;
				emit q->error();
				cleanup();
				return;
			}

			ZurlRequestPacket p;
			p.id = rid.second;
			p.sender = rid.first;
			p.seq = outSeq++;
			p.method = method;
			p.url = url;
			p.headers = headers;
			if(!out.isEmpty() || !outFinished)
				p.more = true;
			p.stream = true;
			p.connectHost = connectHost;
			p.credits = IDEAL_CREDITS;
			manager->write(p);

			if(p.more)
				state = RequestStartWait;
			else
				state = RequestFinishWait;
		}
		else if(state == Requesting)
		{
			tryWrite();
		}
	}

	void timer_timeout()
	{
		// if we're in the middle of stuff, and have received an ack from the server,
		//   then cancel the request
		if(state == Requesting || state == Receiving)
		{
			ZurlRequestPacket p;
			p.id = rid.second;
			p.seq = outSeq++;
			p.cancel = true;
			manager->write(p, replyAddress);
		}

		state = Private::Stopped;
		errorCondition = ZurlRequest::ErrorTimeout;
		cleanup();
		emit q->error();
	}
};

ZurlRequest::ZurlRequest(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZurlRequest::~ZurlRequest()
{
	delete d;
}

ZurlRequest::Rid ZurlRequest::rid() const
{
	return d->rid;
}

void ZurlRequest::setConnectHost(const QString &host)
{
	d->connectHost = host;
}

void ZurlRequest::start(const QString &method, const QUrl &url, const HttpHeaders &headers)
{
	d->state = Private::Starting;
	d->method = method;
	d->url = url;
	d->headers = headers;

	d->timer = new QTimer(d);
	connect(d->timer, SIGNAL(timeout()), d, SLOT(timer_timeout()));
	d->timer->setSingleShot(true);
	d->refreshTimeout();

	d->update();
}

void ZurlRequest::writeBody(const QByteArray &body)
{
	assert(!d->outFinished);

	d->out += body;

	d->update();
}

void ZurlRequest::endBody()
{
	assert(!d->outFinished);

	d->outFinished = true;

	d->update();
}

int ZurlRequest::bytesAvailable() const
{
	return d->in.size();
}

bool ZurlRequest::isFinished() const
{
	return d->state == Private::Stopped;
}

ZurlRequest::ErrorCondition ZurlRequest::errorCondition() const
{
	return d->errorCondition;
}

int ZurlRequest::responseCode() const
{
	return d->responseCode;
}

QByteArray ZurlRequest::responseStatus() const
{
	return d->responseStatus;
}

HttpHeaders ZurlRequest::responseHeaders() const
{
	return d->responseHeaders;
}

QByteArray ZurlRequest::readResponseBody(int size)
{
	return d->readResponseBody(size);
}

void ZurlRequest::setup(ZurlManager *manager)
{
	d->manager = manager;
	d->rid = Rid(manager->clientId(), QUuid::createUuid().toString().toLatin1());
	d->manager->link(this);
}

void ZurlRequest::handle(const ZurlResponsePacket &packet)
{
	d->handle(packet);
}

#include "zurlrequest.moc"
