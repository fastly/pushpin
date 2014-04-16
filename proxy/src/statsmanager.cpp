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

#include "statsmanager.h"

#include <assert.h>
#include <QTimer>
#include <QDateTime>
#include <QPointer>
#include "qzmqsocket.h"
#include "log.h"
#include "tnetstring.h"
#include "packet/statspacket.h"

// make this somewhat big since PUB is lossy
#define OUT_HWM 200000

#define ACTIVITY_TIMEOUT 100
#define CONNECTION_TTL 600
#define CONNECTION_REFRESH 540
#define CONNECTION_LINGER 60
#define REFRESH_TIMEOUT 30000

// FIXME: trickle connection refreshes rather than all at once on an interval

class StatsManager::Private : public QObject
{
	Q_OBJECT

public:
	class ConnectionInfo
	{
	public:
		QByteArray id;
		QByteArray routeId;
		ConnectionType type;
		QHostAddress peerAddress;
		bool ssl;
		QDateTime nextRefresh;
		bool linger;

		ConnectionInfo() :
			ssl(false),
			linger(false)
		{
		}
	};

	StatsManager *q;
	QByteArray instanceId;
	QString spec;
	QZmq::Socket *sock;
	QHash<QByteArray, int> routeActivity;
	QHash<QByteArray, ConnectionInfo*> connectionInfoById;
	QTimer *activityTimer;
	QTimer *refreshTimer;

	Private(StatsManager *_q) :
		QObject(_q),
		q(_q),
		sock(0)
	{
		activityTimer = new QTimer(this);
		connect(activityTimer, SIGNAL(timeout()), SLOT(activity_timeout()));
		activityTimer->setSingleShot(true);

		refreshTimer = new QTimer(this);
		connect(refreshTimer, SIGNAL(timeout()), SLOT(refresh_timeout()));
		refreshTimer->start(REFRESH_TIMEOUT);
	}

	~Private()
	{
		if(activityTimer)
		{
			activityTimer->setParent(0);
			activityTimer->disconnect(this);
			activityTimer->deleteLater();
			activityTimer = 0;
		}

		if(refreshTimer)
		{
			refreshTimer->setParent(0);
			refreshTimer->disconnect(this);
			refreshTimer->deleteLater();
			refreshTimer = 0;
		}

		qDeleteAll(connectionInfoById);
	}

	bool setup()
	{
		delete sock;

		sock = new QZmq::Socket(QZmq::Socket::Pub, this);

		sock->setHwm(OUT_HWM);
		sock->setShutdownWaitTime(0);

		if(!sock->bind(spec))
			return false;

		return true;
	}

	void write(const StatsPacket &packet)
	{
		assert(sock);

		QByteArray prefix;
		if(packet.type == StatsPacket::Activity)
			prefix = "activity ";
		else
			prefix = "conn ";
		QByteArray buf = prefix + TnetString::fromVariant(packet.toVariant());

		log_debug("stats: OUT %s", buf.data());

		sock->write(QList<QByteArray>() << buf);
	}

	void sendActivity(const QByteArray &routeId, int count)
	{
		StatsPacket p;
		p.type = StatsPacket::Activity;
		p.from = instanceId;
		p.route = routeId;
		p.count = count;
		write(p);
	}

	void sendConnected(ConnectionInfo *c)
	{
		StatsPacket p;
		p.type = StatsPacket::Connected;
		p.from = instanceId;
		p.route = c->routeId;
		p.connectionId = c->id;
		if(c->type == WebSocket)
			p.connectionType = StatsPacket::WebSocket;
		else
			p.connectionType = StatsPacket::Http;
		p.peerAddress = c->peerAddress;
		p.ssl = c->ssl;
		p.ttl = CONNECTION_TTL;
		write(p);
	}

	void sendDisconnected(ConnectionInfo *c)
	{
		StatsPacket p;
		p.type = StatsPacket::Disconnected;
		p.from = instanceId;
		p.route = c->routeId;
		p.connectionId = c->id;
		write(p);
	}

private slots:
	void activity_timeout()
	{
		QHashIterator<QByteArray, int> it(routeActivity);
		while(it.hasNext())
		{
			it.next();
			sendActivity(it.key(), it.value());
		}

		routeActivity.clear();
	}

	void refresh_timeout()
	{
		QDateTime now = QDateTime::currentDateTime();

		QList<ConnectionInfo*> toDelete;
		QHashIterator<QByteArray, ConnectionInfo*> it(connectionInfoById);
		while(it.hasNext())
		{
			it.next();
			ConnectionInfo *c = it.value();
			if(now >= c->nextRefresh)
			{
				if(c->linger)
				{
					toDelete += c;

					// note: we don't send a disconnect message when the
					//   linger expires. the assumption is that the handler
					//   owns the connection now
				}
				else
				{
					sendConnected(c);
					c->nextRefresh = now.addSecs(CONNECTION_REFRESH);
				}
			}
		}

		foreach(ConnectionInfo *c, toDelete)
		{
			connectionInfoById.remove(c->id);
			delete c;
		}
	}
};

StatsManager::StatsManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

StatsManager::~StatsManager()
{
	delete d;
}

void StatsManager::setInstanceId(const QByteArray &instanceId)
{
	d->instanceId = instanceId;
}

bool StatsManager::setSpec(const QString &spec)
{
	d->spec = spec;
	return d->setup();
}

void StatsManager::addActivity(const QByteArray &routeId)
{
	if(d->routeActivity.contains(routeId))
		++(d->routeActivity[routeId]);
	else
		d->routeActivity[routeId] = 1;

	if(!d->activityTimer->isActive())
		d->activityTimer->start(ACTIVITY_TIMEOUT);
}

void StatsManager::addConnection(const QByteArray &id, const QByteArray &routeId, ConnectionType type, const QHostAddress &peerAddress, bool ssl, bool quiet)
{
	// if we already had an entry, silently overwrite it. this can
	//   happen if we sent an accepted connection off to the handler,
	//   kept it lingering in our table, and then the handler passed
	//   it back to us for retrying
	Private::ConnectionInfo *c = d->connectionInfoById.value(id);
	if(c)
	{
		d->connectionInfoById.remove(id);
		delete c;
	}

	c = new Private::ConnectionInfo;
	c->id = id;
	c->routeId = routeId;
	c->type = type;
	c->peerAddress = peerAddress;
	c->ssl = ssl;
	d->connectionInfoById[c->id] = c;

	c->nextRefresh = QDateTime::currentDateTime().addSecs(CONNECTION_REFRESH);

	if(!quiet)
		d->sendConnected(c);
}

void StatsManager::removeConnection(const QByteArray &id, bool linger)
{
	Private::ConnectionInfo *c = d->connectionInfoById.value(id);
	if(!c)
		return;

	if(linger)
	{
		if(!c->linger)
		{
			c->linger = true;
			c->nextRefresh = QDateTime::currentDateTime().addSecs(CONNECTION_LINGER);
		}
	}
	else
	{
		d->sendDisconnected(c);
		d->connectionInfoById.remove(id);
		delete c;
	}
}

bool StatsManager::checkConnection(const QByteArray &id)
{
	return d->connectionInfoById.contains(id);
}

#include "statsmanager.moc"
