/*
 * Copyright (C) 2014 Fanout, Inc.
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

#include "zrpcmanager.h"

#include <assert.h>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "log.h"
#include "tnetstring.h"
#include "packet/zrpcrequestpacket.h"
#include "packet/zrpcresponsepacket.h"
#include "zrpcrequest.h"

#define DEFAULT_HWM 5000

class ZrpcManager::Private : public QObject
{
	Q_OBJECT

public:
	ZrpcManager *q;
	QString inSpec;
	QZmq::Socket *inSock;
	QZmq::Valve *inValve;
	QList<ZrpcRequestPacket> pending;

	Private(ZrpcManager *_q) :
		QObject(_q),
		q(_q),
		inSock(0),
		inValve(0)
	{
	}

	bool setup()
	{
		delete inSock;

		inSock = new QZmq::Socket(QZmq::Socket::Rep, this);

		inSock->setHwm(DEFAULT_HWM);

		if(!inSock->bind(inSpec))
			return false;

		inValve = new QZmq::Valve(inSock, this);
		connect(inValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(in_readyRead(const QList<QByteArray> &)));

		inValve->open();

		return true;
	}

	void write(const ZrpcResponsePacket &packet)
	{
		assert(inSock);

		QByteArray buf = TnetString::fromVariant(packet.toVariant());

		log_debug("zrpc: OUT %s", buf.data());

		inSock->write(QList<QByteArray>() << buf);
	}

private slots:
	void in_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("zrpc: received message with parts != 1, skipping");
			return;
		}

		log_debug("zrpc: IN %s", message[0].data());

		QVariant data = TnetString::toVariant(message[0]);
		if(data.isNull())
		{
			log_warning("zrpc: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		ZrpcRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zrpc: received message with invalid format (parse failed), skipping");
			return;
		}

		pending += p;

		emit q->requestReady();
	}
};

ZrpcManager::ZrpcManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZrpcManager::~ZrpcManager()
{
	delete d;
}

bool ZrpcManager::setInSpec(const QString &spec)
{
	d->inSpec = spec;
	return d->setup();
}

ZrpcRequest *ZrpcManager::takeNext()
{
	if(d->pending.isEmpty())
		return 0;

	ZrpcRequestPacket p = d->pending.takeFirst();
	ZrpcRequest *req = new ZrpcRequest;
	req->setup(this);
	req->handle(p);
	return req;
}

void ZrpcManager::write(const ZrpcResponsePacket &packet)
{
	d->write(packet);
}

#include "zrpcmanager.moc"
