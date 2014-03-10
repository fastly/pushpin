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

#include "wscontrolsession.h"

#include <assert.h>
#include <QUuid>
#include <QTimer>
#include "wscontrolmanager.h"

#define KEEPALIVE_TIMEOUT 30000

class WsControlSession::Private : public QObject
{
	Q_OBJECT

public:
	WsControlSession *q;
	WsControlManager *manager;
	QByteArray cid;
	QTimer *keepAliveTimer;
	QByteArray channelPrefix;

	Private(WsControlSession *_q) :
		QObject(_q),
		q(_q),
		manager(0)
	{
		 keepAliveTimer = new QTimer(this);
		 connect(keepAliveTimer, SIGNAL(timeout()), this, SLOT(keepAlive_timeout()));
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(keepAliveTimer)
		{
			keepAliveTimer->disconnect(this);
			keepAliveTimer->setParent(0);
			keepAliveTimer->deleteLater();
			keepAliveTimer = 0;
		}

		if(manager)
		{
			WsControlPacket::Item i;
			i.type = WsControlPacket::Item::Gone;
			write(i);

			manager->unlink(cid);
			manager = 0;
		}
	}

	void start()
	{
		keepAliveTimer->start(KEEPALIVE_TIMEOUT);

		WsControlPacket::Item i;
		i.type = WsControlPacket::Item::Here;
		i.channelPrefix = channelPrefix;
		write(i);
	}

	void sendGripMessage(const QByteArray &message)
	{
		WsControlPacket::Item i;
		i.type = WsControlPacket::Item::Grip;
		i.message = message;
		write(i);
	}

	void write(const WsControlPacket::Item &item)
	{
		WsControlPacket::Item out = item;
		out.cid = cid;
		manager->write(out);
	}

	void handle(const WsControlPacket::Item &item)
	{
		if(item.type == WsControlPacket::Item::Send)
		{
			QByteArray contentType;
			if(!item.contentType.isEmpty())
				contentType = item.contentType;
			else
				contentType = "text";

			emit q->sendEventReceived(contentType, item.message);
		}
		else if(item.type == WsControlPacket::Item::Detach)
		{
			emit q->detachEventReceived();
		}
	}

private slots:
	void keepAlive_timeout()
	{
		WsControlPacket::Item i;
		i.type = WsControlPacket::Item::Here;
		write(i);
	}
};

WsControlSession::WsControlSession(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

WsControlSession::~WsControlSession()
{
	delete d;
}

void WsControlSession::start(const QByteArray &channelPrefix)
{
	d->channelPrefix = channelPrefix;
	d->start();
}

void WsControlSession::sendGripMessage(const QByteArray &message)
{
	d->sendGripMessage(message);
}

void WsControlSession::setup(WsControlManager *manager)
{
	d->manager = manager;
	d->cid = QUuid::createUuid().toString().toLatin1();
	d->manager->link(this, d->cid);
}

void WsControlSession::handle(const WsControlPacket::Item &item)
{
	assert(d->manager);

	d->handle(item);
}

#include "wscontrolsession.moc"
