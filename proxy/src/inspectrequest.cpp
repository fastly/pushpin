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

#include "inspectrequest.h"

#include <QUuid>
#include "packet/inspectrequestpacket.h"
#include "packet/inspectresponsepacket.h"
#include "packet/httprequestdata.h"
#include "inspectdata.h"
#include "inspectmanager.h"

class InspectRequest::Private : public QObject
{
	Q_OBJECT

public:
	InspectRequest *q;
	InspectManager *manager;
	QByteArray id;

	Private(InspectRequest *_q) :
		QObject(_q),
		q(_q),
		manager(0)
	{
	}
};

InspectRequest::InspectRequest(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

InspectRequest::~InspectRequest()
{
	delete d;
}

void InspectRequest::start(const HttpRequestData &hdata)
{
	InspectRequestPacket p;
	p.id = d->id;
	p.method = hdata.method;
	p.path = hdata.path;
	p.headers = hdata.headers;

	d->manager->write(p);
}

QByteArray InspectRequest::id() const
{
	return d->id;
}

void InspectRequest::setup(InspectManager *manager)
{
	d->id = QUuid::createUuid().toString().toLatin1();
	d->manager = manager;
}

void InspectRequest::handle(const InspectResponsePacket &packet)
{
	InspectData idata;
	idata.doProxy = !packet.noProxy;
	idata.sharingKey = packet.sharingKey;
	idata.userData = packet.userData;
	emit finished(idata);
}

#include "inspectrequest.moc"
