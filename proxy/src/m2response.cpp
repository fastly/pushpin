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

#include "m2response.h"

#include "packet/m2responsepacket.h"
#include "m2manager.h"

static QByteArray makeChunk(const QByteArray &in)
{
	QByteArray out;
	out += QByteArray::number(in.size(), 16).toUpper() + "\r\n";
	out += in;
	out += "\r\n";
	return out;
}

class M2Response::Private
{
public:
	M2Manager *manager;
	M2Request::Rid rid;

	Private() :
		manager(0)
	{
	}
};

M2Response::M2Response(QObject *parent) :
	QObject(parent)
{
	d = new Private;
}

M2Response::~M2Response()
{
	delete d;
}

void M2Response::write(int code, const QByteArray &status, const HttpHeaders &headers, const QByteArray &body, bool chunked)
{
	M2ResponsePacket p;
	p.sender = d->rid.first;
	p.id = d->rid.second;
	p.data = "HTTP/1.1 " + QByteArray::number(code) + ' ' + status + "\r\n";
	foreach(const HttpHeader &h, headers)
		p.data += h.first + ": " + h.second + "\r\n";
	p.data += "\r\n";
	if(chunked)
		p.data += makeChunk(body);
	else
		p.data += body;
	d->manager->writeResponse(p);

	//QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection);
}

void M2Response::write(const QByteArray &body, bool chunked)
{
	M2ResponsePacket p;
	p.sender = d->rid.first;
	p.id = d->rid.second;
	if(chunked)
		p.data = makeChunk(body);
	else
		p.data = body;

	d->manager->writeResponse(p);
}

void M2Response::handle(M2Manager *manager, const M2Request::Rid &rid)
{
	d->manager = manager;
	d->rid = rid;
}
