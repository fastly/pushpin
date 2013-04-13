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

#include "m2manager.h"

#include "assert.h"
#include <QSet>
#include <QStringList>
#include <QObject>
#include <QPointer>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "packet/m2requestpacket.h"
#include "packet/m2responsepacket.h"
#include "log.h"
#include "m2request.h"
#include "m2response.h"

#define INCOMING_HWM 100
#define DEFAULT_HWM 1000
#define MAX_PENDING 100

class M2Manager::Private : public QObject
{
	Q_OBJECT

public:
	M2Manager *q;
	QStringList in_specs;
	QStringList inhttps_specs;
	QStringList out_specs;
	QZmq::Socket *in_sock;
	QZmq::Socket *inhttps_sock;
	QZmq::Socket *out_sock;
	QZmq::Valve *in_valve;
	QZmq::Valve *inhttps_valve;
	QHash<M2Request::Rid, M2Request*> reqsByRid;
	QHash<M2Request::Rid, QSet<M2Response*> > respsByRid;
	QList<M2Request*> pendingReqs;

	Private(M2Manager *_q) :
		QObject(_q),
		q(_q),
		in_sock(0),
		inhttps_sock(0),
		out_sock(0),
		in_valve(0),
		inhttps_valve(0)
	{
	}

	bool setupIncomingPlain()
	{
		if(in_sock)
		{
			delete in_valve;
			in_valve = 0;
			delete in_sock;
			in_sock = 0;
		}

		in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);

		in_sock->setHwm(INCOMING_HWM);

		foreach(const QString &spec, in_specs)
			in_sock->connectToAddress(spec);

		in_valve = new QZmq::Valve(in_sock, this);
		connect(in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(in_readyRead(const QList<QByteArray> &)));

		in_valve->open();

		return true;
	}

	bool setupIncomingHttps()
	{
		if(inhttps_sock)
		{
			delete inhttps_valve;
			inhttps_valve = 0;
			delete inhttps_sock;
			inhttps_sock = 0;
		}

		inhttps_sock = new QZmq::Socket(QZmq::Socket::Pull, this);

		inhttps_sock->setHwm(INCOMING_HWM);

		foreach(const QString &spec, inhttps_specs)
			inhttps_sock->connectToAddress(spec);

		inhttps_valve = new QZmq::Valve(inhttps_sock, this);
		connect(inhttps_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(inhttps_readyRead(const QList<QByteArray> &)));

		inhttps_valve->open();

		return true;
	}

	bool setupOutgoing()
	{
		if(out_sock)
		{
			delete out_sock;
			out_sock = 0;
		}

		out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
		connect(out_sock, SIGNAL(messagesWritten(int)), SLOT(out_messagesWritten(int)));

		out_sock->setWriteQueueEnabled(false);
		out_sock->setHwm(DEFAULT_HWM);

		foreach(const QString &spec, out_specs)
			out_sock->connectToAddress(spec);

		return true;
	}

	void handleIncoming(const QList<QByteArray> &message, bool https)
	{
		if(message.count() != 1)
		{
			log_warning("m2manager: received message with parts != 1, skipping");
			return;
		}

		M2RequestPacket p;
		if(!p.fromByteArray(message[0]))
		{
			log_warning("m2manager: received message with invalid format, skipping");
			return;
		}

		M2Request::Rid rid(p.sender, p.id);
		M2Request *req;

		if(p.isDisconnect)
		{
			QPointer<QObject> self = this;

			log_debug("m2manager: id=%s disconnected", rid.second.data());

			req = reqsByRid.value(rid);
			if(req)
			{
				req->disconnected();
				if(!self)
					return;
			}

			foreach(M2Response *resp, respsByRid.value(rid))
			{
				resp->disconnected();
				if(!self)
					return;
			}

			return;
		}

		if(p.uploadDone)
		{
			req = reqsByRid.value(rid);
			if(!req)
			{
				log_warning("m2manager: upload finished of unknown request, skipping");
				return;
			}

			req->uploadDone();
			return;
		}

		// newer mongrel2's tell us this
		if(p.scheme == "https")
			https = true;

		req = new M2Request;

		reqsByRid.insert(rid, req);

		if(!req->handle(q, p, https))
		{
			// we don't log anything here. req will have taken care of that
			reqsByRid.remove(rid);
			delete req;
			return;
		}

		pendingReqs += req;

		if(pendingReqs.count() >= MAX_PENDING)
		{
			if(in_valve)
				in_valve->close();
			if(inhttps_valve)
				inhttps_valve->close();
		}

		emit q->requestReady();
	}

private slots:
	void in_readyRead(const QList<QByteArray> &message)
	{
		handleIncoming(message, false);
	}

	void inhttps_readyRead(const QList<QByteArray> &message)
	{
		handleIncoming(message, true);
	}

	void out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}
};

M2Manager::M2Manager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

M2Manager::~M2Manager()
{
	delete d;
}

bool M2Manager::setIncomingPlainSpecs(const QStringList &specs)
{
	d->in_specs = specs;
	return d->setupIncomingPlain();
}

bool M2Manager::setIncomingHttpsSpecs(const QStringList &specs)
{
	d->inhttps_specs = specs;
	return d->setupIncomingHttps();
}

bool M2Manager::setOutgoingSpecs(const QStringList &specs)
{
	d->out_specs = specs;
	return d->setupOutgoing();
}

M2Request *M2Manager::takeNext()
{
	if(d->pendingReqs.isEmpty())
		return 0;

	if(d->in_valve)
		d->in_valve->open();
	if(d->inhttps_valve)
		d->inhttps_valve->open();

	M2Request *req = d->pendingReqs.takeFirst();
	req->activate();
	return req;
}

M2Response *M2Manager::createResponse(const M2Request::Rid &rid)
{
	M2Response *resp = new M2Response;

	if(!d->respsByRid.contains(resp->rid()))
		d->respsByRid[rid] = QSet<M2Response*>();

	QSet<M2Response*> &resps = d->respsByRid[rid];
	resps += resp;

	resp->handle(this, rid);
	return resp;
}

void M2Manager::unlink(M2Request *req)
{
	d->reqsByRid.remove(req->rid());
}

void M2Manager::unlink(M2Response *resp)
{
	if(d->respsByRid.contains(resp->rid()))
	{
		QSet<M2Response*> &resps = d->respsByRid[resp->rid()];
		resps.remove(resp);

		if(resps.isEmpty())
			d->respsByRid.remove(resp->rid());
	}
}

void M2Manager::writeResponse(const M2ResponsePacket &packet)
{
	assert(d->out_sock);

	d->out_sock->write(QList<QByteArray>() << packet.toByteArray());
}

#include "m2manager.moc"
