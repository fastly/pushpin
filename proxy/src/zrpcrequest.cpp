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

#include "zrpcrequest.h"

#include <assert.h>
#include "packet/zrpcrequestpacket.h"
#include "packet/zrpcresponsepacket.h"
#include "zrpcmanager.h"

class ZrpcRequest::Private : public QObject
{
	Q_OBJECT

public:
	ZrpcRequest *q;
	ZrpcManager *manager;
	QByteArray id;
	QString method;
	QVariantHash args;

	Private(ZrpcRequest *_q) :
		QObject(_q),
		q(_q),
		manager(0)
	{
	}

	void respond(const QVariant &value)
	{
		ZrpcResponsePacket p;
		p.id = id;
		p.success = true;
		p.value = value;
		manager->write(p);
	}

	void respondError(const QByteArray &condition)
	{
		ZrpcResponsePacket p;
		p.id = id;
		p.success = false;
		p.condition = condition;
		manager->write(p);
	}

	void handle(const ZrpcRequestPacket &packet)
	{
		id = packet.id;
		method = packet.method;
		args = packet.args;
	}
};

ZrpcRequest::ZrpcRequest(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZrpcRequest::~ZrpcRequest()
{
	delete d;
}

QString ZrpcRequest::method() const
{
	return d->method;
}

QVariantHash ZrpcRequest::args() const
{
	return d->args;
}

void ZrpcRequest::respond(const QVariant &value)
{
	d->respond(value);
}

void ZrpcRequest::respondError(const QByteArray &condition)
{
	d->respondError(condition);
}

void ZrpcRequest::setup(ZrpcManager *manager)
{
	d->manager = manager;
}

void ZrpcRequest::handle(const ZrpcRequestPacket &packet)
{
	assert(d->manager);

	d->handle(packet);
}

#include "zrpcrequest.moc"
