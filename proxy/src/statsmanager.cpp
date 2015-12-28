/*
 * Copyright (C) 2014-2015 Fanout, Inc.
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
#include "zutil.h"

// make this somewhat big since PUB is lossy
#define OUT_HWM 200000

#define ACTIVITY_TIMEOUT 100
#define CONNECTION_TTL 600
#define CONNECTION_REFRESH (CONNECTION_TTL * 9 / 10)
#define CONNECTION_LINGER 60
#define SUBSCRIPTION_TTL 60
#define SUBSCRIPTION_REFRESH (SUBSCRIPTION_TTL * 9 / 10)
#define SUBSCRIPTION_LINGER 60
#define REFRESH_TIMEOUT (3 * 1000)

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

	class Subscription
	{
	public:
		QString mode;
		QString channel;
		QDateTime nextRefresh;
		bool linger;

		Subscription() :
			linger(false)
		{
		}
	};

	typedef QPair<QString, QString> SubscriptionKey;

	StatsManager *q;
	QByteArray instanceId;
	int ipcFileMode;
	QString spec;
	QZmq::Socket *sock;
	QHash<QByteArray, int> routeActivity;
	QHash<QByteArray, ConnectionInfo*> connectionInfoById;
	QHash<SubscriptionKey, Subscription*> subscriptionsByKey;
	QTimer *activityTimer;
	QTimer *refreshTimer;

	Private(StatsManager *_q) :
		QObject(_q),
		q(_q),
		ipcFileMode(-1),
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

		QString errorMessage;
		if(!ZUtil::setupSocket(sock, spec, true, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	void write(const StatsPacket &packet)
	{
		assert(sock);

		QByteArray prefix;
		if(packet.type == StatsPacket::Activity)
			prefix = "activity";
		else if(packet.type == StatsPacket::Message)
			prefix = "message";
		else if(packet.type == StatsPacket::Connected || packet.type == StatsPacket::Disconnected)
			prefix = "conn";
		else // Subscribed/Unsubscribed
			prefix = "sub";

		QVariant vpacket = packet.toVariant();
		QByteArray buf = prefix + " T" + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("stats: OUT %s %s", prefix.data(), qPrintable(TnetString::variantToString(vpacket, -1)));

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

	void sendMessage(const QByteArray &routeId, const QString &channel, const QString &itemId, const QString &transport, int count)
	{
		StatsPacket p;
		p.type = StatsPacket::Message;
		p.from = instanceId;
		p.route = routeId;
		p.channel = channel.toUtf8();
		p.itemId = itemId.toUtf8();
		p.count = count;
		p.transport = transport.toUtf8();
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

	void sendSubscribed(Subscription *s)
	{
		StatsPacket p;
		p.type = StatsPacket::Subscribed;
		p.mode = s->mode.toUtf8();
		p.channel = s->channel.toUtf8();
		p.ttl = SUBSCRIPTION_TTL;
		write(p);
	}

	void sendUnsubscribed(Subscription *s)
	{
		StatsPacket p;
		p.type = StatsPacket::Unsubscribed;
		p.mode = s->mode.toUtf8();
		p.channel = s->channel.toUtf8();
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

		{
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

		{
			QList<Subscription*> toDelete;
			QHashIterator<SubscriptionKey, Subscription*> it(subscriptionsByKey);
			while(it.hasNext())
			{
				it.next();
				Subscription *s = it.value();
				if(now >= s->nextRefresh)
				{
					if(s->linger)
					{
						toDelete += s;

						sendUnsubscribed(s);
					}
					else
					{
						sendSubscribed(s);
						s->nextRefresh = now.addSecs(SUBSCRIPTION_REFRESH);
					}
				}
			}

			foreach(Subscription *s, toDelete)
			{
				SubscriptionKey subKey(s->mode, s->channel);
				subscriptionsByKey.remove(subKey);
				delete s;

				emit q->unsubscribed(subKey.first, subKey.second);
			}
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

void StatsManager::setIpcFileMode(int mode)
{
	d->ipcFileMode = mode;
}

bool StatsManager::setSpec(const QString &spec)
{
	d->spec = spec;
	return d->setup();
}

void StatsManager::addActivity(const QByteArray &routeId, int count)
{
	assert(count >= 0);

	if(d->routeActivity.contains(routeId))
		d->routeActivity[routeId] += count;
	else
		d->routeActivity[routeId] = count;

	if(!d->activityTimer->isActive())
		d->activityTimer->start(ACTIVITY_TIMEOUT);
}

void StatsManager::addMessage(const QByteArray &routeId, const QString &channel, const QString &itemId, const QString &transport, int count)
{
	d->sendMessage(routeId, channel, itemId, transport, count);
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

void StatsManager::addSubscription(const QString &mode, const QString &channel)
{
	Private::SubscriptionKey subKey(mode, channel);
	Private::Subscription *s = d->subscriptionsByKey.value(subKey);
	if(!s)
	{
		// add the subscription if we didn't have it
		s = new Private::Subscription;
		s->mode = mode;
		s->channel = channel;
		d->subscriptionsByKey[subKey] = s;

		s->nextRefresh = QDateTime::currentDateTime().addSecs(SUBSCRIPTION_REFRESH);
		d->sendSubscribed(s);
	}
	else
	{
		if(s->linger)
		{
			// if this was a lingering subscription, return it to normal
			s->linger = false;
			s->nextRefresh = QDateTime::currentDateTime().addSecs(SUBSCRIPTION_REFRESH);
			d->sendSubscribed(s);
		}
	}
}

void StatsManager::removeSubscription(const QString &mode, const QString &channel, bool linger)
{
	Private::SubscriptionKey subKey(mode, channel);
	Private::Subscription *s = d->subscriptionsByKey.value(subKey);
	if(!s)
		return;

	if(linger)
	{
		if(!s->linger)
		{
			s->linger = true;
			s->nextRefresh = QDateTime::currentDateTime().addSecs(SUBSCRIPTION_LINGER);
		}
	}
	else
	{
		d->sendUnsubscribed(s);
		d->subscriptionsByKey.remove(subKey);
		delete s;

		emit unsubscribed(mode, channel);
	}
}

bool StatsManager::checkConnection(const QByteArray &id)
{
	return d->connectionInfoById.contains(id);
}

void StatsManager::sendPacket(const StatsPacket &packet)
{
	StatsPacket p = packet;
	p.from = d->instanceId;
	d->write(p);
}

#include "statsmanager.moc"
