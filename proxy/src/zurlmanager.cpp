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

#include "zurlmanager.h"

#include <assert.h>
#include <QStringList>
#include <QHash>
#include <QPointer>
#include "qzmqsocket.h"
#include "packet/tnetstring.h"
#include "packet/zurlrequestpacket.h"
#include "packet/zurlresponsepacket.h"
#include "log.h"
#include "zurlrequest.h"

#define REQUEST_HWM 100
#define DEFAULT_HWM 1000

class ZurlManager::Private : public QObject
{
	Q_OBJECT

public:
	ZurlManager *q;
	QStringList out_specs;
	QStringList out_stream_specs;
	QStringList in_specs;
	QZmq::Socket *out_sock;
	QZmq::Socket *out_stream_sock;
	QZmq::Socket *in_sock;
	QByteArray clientId;
	QHash<ZurlRequest::Rid, ZurlRequest*> reqsByRid;

	Private(ZurlManager *_q) :
		QObject(_q),
		q(_q),
		out_sock(0),
		out_stream_sock(0),
		in_sock(0)
	{
	}

	bool setupOutgoing()
	{
		delete out_sock;

		out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
		connect(out_sock, SIGNAL(messagesWritten(int)), SLOT(out_messagesWritten(int)));

		out_sock->setHwm(REQUEST_HWM);

		foreach(const QString &spec, out_specs)
			out_sock->connectToAddress(spec);

		return true;
	}

	bool setupOutgoingStream()
	{
		delete out_stream_sock;

		out_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);
		connect(out_stream_sock, SIGNAL(messagesWritten(int)), SLOT(out_stream_messagesWritten(int)));

		out_stream_sock->setWriteQueueEnabled(false);
		out_stream_sock->setHwm(DEFAULT_HWM);

		foreach(const QString &spec, out_stream_specs)
			out_stream_sock->connectToAddress(spec);

		return true;
	}

	bool setupIncoming()
	{
		delete in_sock;

		in_sock = new QZmq::Socket(QZmq::Socket::Sub, this);
		connect(in_sock, SIGNAL(readyRead()), SLOT(in_readyRead()));

		in_sock->setHwm(DEFAULT_HWM);

		foreach(const QString &spec, in_specs)
		{
			in_sock->subscribe(clientId + ' ');
			in_sock->connectToAddress(spec);
		}

		return true;
	}

public slots:
	void out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void out_stream_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void in_readyRead()
	{
		QPointer<QObject> self = this;

		while(in_sock->canRead())
		{
			QList<QByteArray> msg = in_sock->read();
			if(msg.count() != 1)
			{
				log_warning("zurlmanager: received message with parts != 1, skipping");
				continue;
			}

			int at = msg[0].indexOf(' ');
			if(at == -1)
			{
				log_warning("zurlmanager: received message with invalid format, skipping");
				continue;
			}

			QByteArray receiver = msg[0].mid(0, at);
			QVariant data = TnetString::toVariant(msg[0].mid(at + 1));
			if(data.isNull())
			{
				log_warning("zurlmanager: received message with invalid format (tnetstring parse failed), skipping");
				continue;
			}

			ZurlResponsePacket p;
			if(!p.fromVariant(data))
			{
				log_warning("zurlmanager: received message with invalid format (parse failed), skipping");
				continue;
			}

			ZurlRequest *req = reqsByRid.value(ZurlRequest::Rid(clientId, p.id));
			if(!req)
			{
				log_warning("zurlmanager: received message for unknown request id, canceling");

				// if this was not an error packet, send cancel
				if(p.condition.isEmpty() && !p.replyAddress.isEmpty())
				{
					ZurlRequestPacket out;
					out.id = p.id;
					out.cancel = true;
					q->write(out, p.replyAddress);
				}

				continue;
			}

			req->handle(p);

			if(!self)
				return;
		}
	}
};

ZurlManager::ZurlManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZurlManager::~ZurlManager()
{
	delete d;
}

QByteArray ZurlManager::clientId() const
{
	return d->clientId;
}

void ZurlManager::setClientId(const QByteArray &id)
{
	d->clientId = id;
}

bool ZurlManager::setOutgoingSpecs(const QStringList &specs)
{
	d->out_specs = specs;
	return d->setupOutgoing();
}

bool ZurlManager::setOutgoingStreamSpecs(const QStringList &specs)
{
	d->out_stream_specs = specs;
	return d->setupOutgoingStream();
}

bool ZurlManager::setIncomingSpecs(const QStringList &specs)
{
	d->in_specs = specs;
	return d->setupIncoming();
}

ZurlRequest *ZurlManager::createRequest()
{
	ZurlRequest *req = new ZurlRequest;
	req->setup(this);
	return req;
}

void ZurlManager::link(ZurlRequest *req)
{
	d->reqsByRid.insert(req->rid(), req);
}

void ZurlManager::unlink(ZurlRequest *req)
{
	d->reqsByRid.remove(req->rid());
}

bool ZurlManager::canWriteImmediately() const
{
	assert(d->out_sock);

	return d->out_sock->canWriteImmediately();
}

void ZurlManager::write(const ZurlRequestPacket &packet)
{
	assert(d->out_sock);

	QByteArray buf = TnetString::fromVariant(packet.toVariant());
	log_debug("zurlmanager write: id=%s seq=%d", packet.id.data(), packet.seq);

	d->out_sock->write(QList<QByteArray>() << buf);
}

void ZurlManager::write(const ZurlRequestPacket &packet, const QByteArray &instanceAddress)
{
	assert(d->out_sock);

	QByteArray buf = TnetString::fromVariant(packet.toVariant());
	log_debug("zurlmanager write: to=%s id=%s seq=%d", instanceAddress.data(), packet.id.data(), packet.seq);

	QList<QByteArray> msg;
	msg += instanceAddress;
	msg += QByteArray();
	msg += buf;
	d->out_stream_sock->write(msg);
}

#include "zurlmanager.moc"
