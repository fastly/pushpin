/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#include <QTimer>
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
	HttpRequestData hdata;
	InspectRequest::ErrorCondition condition;
	QTimer *timer;

	Private(InspectRequest *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		timer(0)
	{
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

		if(manager)
		{
			manager->unlink(q);
			manager = 0;
		}
	}

	void handle(const InspectResponsePacket &packet)
	{
		InspectData idata;
		idata.doProxy = !packet.noProxy;
		idata.sharingKey = packet.sharingKey;
		idata.userData = packet.userData;
		cleanup();
		emit q->finished(idata);
	}

public slots:
	void doStart()
	{
		if(!manager->canWriteImmediately())
		{
			condition = InspectRequest::ErrorUnavailable;
			cleanup();
			emit q->error();
			return;
		}

		InspectRequestPacket p;
		p.id = id;
		p.method = hdata.method;
		p.uri = hdata.uri;
		p.headers = hdata.headers;

		if(manager->timeout() >= 0)
		{
			timer = new QTimer(this);
			connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
			timer->setSingleShot(true);
			timer->start(manager->timeout());
		}

		manager->write(p);
	}

	void timer_timeout()
	{
		condition = InspectRequest::ErrorTimeout;
		cleanup();
		emit q->error();
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

InspectRequest::ErrorCondition InspectRequest::errorCondition() const
{
	return d->condition;
}

void InspectRequest::start(const HttpRequestData &hdata)
{
	d->hdata = hdata;
	QMetaObject::invokeMethod(d, "doStart", Qt::QueuedConnection);
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
	d->handle(packet);
}

#include "inspectrequest.moc"
