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
#define CONNECTION_TTL 120
#define CONNECTION_REFRESH (CONNECTION_TTL * 3 / 4)
#define CONNECTION_LINGER 60
#define SUBSCRIPTION_TTL 60
#define SUBSCRIPTION_REFRESH (SUBSCRIPTION_TTL * 3 / 4)
#define SUBSCRIPTION_LINGER 60
#define REPORT_TIMEOUT 10000
#define REFRESH_TIMEOUT (5 * 1000)

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
		QDateTime lastReported;
		QByteArray from; // external
		int ttl; // external
		QDateTime lastUpdated; // external

		ConnectionInfo() :
			ssl(false),
			linger(false),
			ttl(-1)
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

	class Report
	{
	public:
		QByteArray routeId;
		int connectionsMax;
		bool connectionsMaxStale;
		int connectionsMinutes;
		int messagesReceived;
		int messagesSent;
		int httpResponseMessagesSent;
		QDateTime lastUpdated;

		Report() :
			connectionsMax(0),
			connectionsMaxStale(true),
			connectionsMinutes(0),
			messagesReceived(0),
			messagesSent(0),
			httpResponseMessagesSent(0)
		{
		}

		bool isEmpty() const
		{
			return (connectionsMax == 0 &&
				connectionsMinutes == 0 &&
				messagesReceived == 0 &&
				messagesSent == 0 &&
				httpResponseMessagesSent == 0);
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
	QHash<QByteArray, QSet<ConnectionInfo*> > connectionInfoByRoute;
	QHash<QByteArray, QHash<QByteArray, ConnectionInfo*> > externalConnectionInfoByFrom;
	QHash<QByteArray, QSet<ConnectionInfo*> > externalConnectionInfoByRoute;
	QHash<SubscriptionKey, Subscription*> subscriptionsByKey;
	QHash<QByteArray, Report*> reports;
	QTimer *activityTimer;
	QTimer *refreshTimer;
	QTimer *reportTimer;
	bool reportsEnabled;

	Private(StatsManager *_q) :
		QObject(_q),
		q(_q),
		ipcFileMode(-1),
		sock(0),
		reportTimer(0),
		reportsEnabled(false)
	{
		activityTimer = new QTimer(this);
		connect(activityTimer, &QTimer::timeout, this, &Private::activity_timeout);
		activityTimer->setSingleShot(true);

		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, &Private::refresh_timeout);
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

		if(reportTimer)
		{
			reportTimer->setParent(0);
			reportTimer->disconnect(this);
			reportTimer->deleteLater();
			reportTimer = 0;
		}

		qDeleteAll(connectionInfoById);

		QMutableHashIterator<QByteArray, QHash<QByteArray, ConnectionInfo*> > it(externalConnectionInfoByFrom);
		while(it.hasNext())
		{
			it.next();
			qDeleteAll(it.value());
		}

		qDeleteAll(subscriptionsByKey);

		qDeleteAll(reports);
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

	void setupReports()
	{
		if(reportsEnabled && !reportTimer)
		{
			reportTimer = new QTimer(this);
			connect(reportTimer, &QTimer::timeout, this, &Private::report_timeout);
			reportTimer->start(REPORT_TIMEOUT);
		}
		else if(!reportsEnabled && reportTimer)
		{
			reportTimer->setParent(0);
			reportTimer->disconnect(this);
			reportTimer->deleteLater();
			reportTimer = 0;
		}
	}

	void insertConnection(ConnectionInfo *c)
	{
		connectionInfoById[c->id] = c;

		QSet<ConnectionInfo*> &cs = connectionInfoByRoute[c->routeId];
		cs += c;
	}

	void removeConnection(ConnectionInfo *c)
	{
		connectionInfoById.remove(c->id);

		if(connectionInfoByRoute.contains(c->routeId))
		{
			QSet<ConnectionInfo*> &cs = connectionInfoByRoute[c->routeId];
			cs.remove(c);

			if(cs.isEmpty())
				connectionInfoByRoute.remove(c->routeId);
		}
	}

	void insertExternalConnection(ConnectionInfo *c)
	{
		QHash<QByteArray, ConnectionInfo*> &extConnectionInfoById = externalConnectionInfoByFrom[c->from];
		extConnectionInfoById[c->id] = c;

		QSet<ConnectionInfo*> &cs = externalConnectionInfoByRoute[c->routeId];
		cs += c;
	}

	void removeExternalConnection(ConnectionInfo *c)
	{
		if(externalConnectionInfoByFrom.contains(c->from))
		{
			QHash<QByteArray, ConnectionInfo*> &extConnectionInfoById = externalConnectionInfoByFrom[c->from];
			extConnectionInfoById.remove(c->id);

			if(extConnectionInfoById.isEmpty())
				externalConnectionInfoByFrom.remove(c->from);
		}

		if(externalConnectionInfoByRoute.contains(c->routeId))
		{
			QSet<ConnectionInfo*> &cs = externalConnectionInfoByRoute[c->routeId];
			cs.remove(c);

			if(cs.isEmpty())
				externalConnectionInfoByRoute.remove(c->routeId);
		}
	}

	Report *getOrCreateReport(const QByteArray &routeId)
	{
		Report *report = reports.value(routeId);
		if(!report)
		{
			report = new Report;
			report->routeId = routeId;
			reports[routeId] = report;
		}

		return report;
	}

	void removeReport(Report *report)
	{
		reports.remove(report->routeId);
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
		else if(packet.type == StatsPacket::Subscribed || packet.type == StatsPacket::Unsubscribed)
			prefix = "sub";
		else // Report
			prefix = "report";

		QVariant vpacket = packet.toVariant();
		QByteArray buf = prefix + " T" + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("stats: OUT %s %s", prefix.data(), qPrintable(TnetString::variantToString(vpacket, -1)));

		sock->write(QList<QByteArray>() << buf);
	}

	void sendActivity(const QByteArray &routeId, int count)
	{
		if(!sock)
			return;

		StatsPacket p;
		p.type = StatsPacket::Activity;
		p.from = instanceId;
		p.route = routeId;
		p.count = count;
		write(p);
	}

	void sendMessage(const QString &channel, const QString &itemId, const QString &transport, int count)
	{
		if(!sock)
			return;

		StatsPacket p;
		p.type = StatsPacket::Message;
		p.from = instanceId;
		p.channel = channel.toUtf8();
		p.itemId = itemId.toUtf8();
		p.count = count;
		p.transport = transport.toUtf8();
		write(p);
	}

	void sendConnected(ConnectionInfo *c)
	{
		if(!sock)
			return;

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
		if(!sock)
			return;

		StatsPacket p;
		p.type = StatsPacket::Disconnected;
		p.from = instanceId;
		p.route = c->routeId;
		p.connectionId = c->id;
		write(p);
	}

	void sendSubscribed(Subscription *s)
	{
		if(!sock)
			return;

		StatsPacket p;
		p.type = StatsPacket::Subscribed;
		p.from = instanceId;
		p.mode = s->mode.toUtf8();
		p.channel = s->channel.toUtf8();
		p.ttl = SUBSCRIPTION_TTL;
		write(p);
	}

	void sendUnsubscribed(Subscription *s)
	{
		if(!sock)
			return;

		StatsPacket p;
		p.type = StatsPacket::Unsubscribed;
		p.from = instanceId;
		p.mode = s->mode.toUtf8();
		p.channel = s->channel.toUtf8();
		write(p);
	}

	void updateConnectionsMax(const QByteArray &routeId)
	{
		Report *report = getOrCreateReport(routeId);

		int localConns = connectionInfoByRoute.value(routeId).count();
		int extConns = externalConnectionInfoByRoute.value(routeId).count();

		log_debug("updateConnectionsMax: %d/%d", localConns, extConns);

		int conns = localConns + extConns;

		if(report->connectionsMaxStale)
		{
			report->connectionsMax = conns;
			report->connectionsMaxStale = false;
		}
		else
			report->connectionsMax = qMax(report->connectionsMax, conns);

		report->lastUpdated = QDateTime::currentDateTimeUtc();
	}

	void updateConnectionsMinutes(ConnectionInfo *c, const QDateTime &now)
	{
		// ignore lingering connections
		if(c->linger)
			return;

		Report *report = getOrCreateReport(c->routeId);

		int mins = c->lastReported.secsTo(now) / 60;
		if(mins > 0)
		{
			// only advance as much as we've read
			c->lastReported = c->lastReported.addSecs(mins * 60);

			report->connectionsMinutes += mins;

			report->lastUpdated = QDateTime::currentDateTimeUtc();
		}
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
		QDateTime now = QDateTime::currentDateTimeUtc();

		{
			QList<QByteArray> refreshedIds;
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

						refreshedIds += c->id;
					}
				}
			}

			foreach(ConnectionInfo *c, toDelete)
			{
				removeConnection(c);
				delete c;
			}

			if(!refreshedIds.isEmpty())
				emit q->connectionsRefreshed(refreshedIds);
		}

		// remove any expired external connections
		if(reportsEnabled)
		{
			QList<ConnectionInfo*> toDelete;

			QHashIterator<QByteArray, QHash<QByteArray, ConnectionInfo*> > it(externalConnectionInfoByFrom);
			while(it.hasNext())
			{
				it.next();
				const QHash<QByteArray, ConnectionInfo*> &extConnectionInfoById = it.value();

				QHashIterator<QByteArray, ConnectionInfo*> cit(extConnectionInfoById);
				while(cit.hasNext())
				{
					cit.next();
					ConnectionInfo *c = cit.value();

					if(now >= c->lastUpdated.addSecs(c->ttl))
						toDelete += c;
				}
			}

			QSet<QByteArray> routesUpdated;
			foreach(ConnectionInfo *c, toDelete)
			{
				routesUpdated += c->routeId;
				updateConnectionsMinutes(c, now);
				removeExternalConnection(c);
				delete c;
			}

			foreach(const QByteArray &routeId, routesUpdated)
				updateConnectionsMax(routeId);
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

	void report_timeout()
	{
		QDateTime now = QDateTime::currentDateTimeUtc();

		// apply recent minutes
		{
			QHashIterator<QByteArray, ConnectionInfo*> it(connectionInfoById);
			while(it.hasNext())
			{
				it.next();
				ConnectionInfo *c = it.value();

				updateConnectionsMinutes(c, now);
			}

			QHashIterator<QByteArray, QHash<QByteArray, ConnectionInfo*> > eit(externalConnectionInfoByFrom);
			while(eit.hasNext())
			{
				eit.next();
				const QHash<QByteArray, ConnectionInfo*> &extConnectionInfoById = eit.value();

				QHashIterator<QByteArray, ConnectionInfo*> cit(extConnectionInfoById);
				while(cit.hasNext())
				{
					cit.next();
					ConnectionInfo *c = cit.value();

					updateConnectionsMinutes(c, now);
				}
			}
		}

		{
			QList<StatsPacket> reportPackets;
			QList<Report*> toDelete;

			QHashIterator<QByteArray, Report*> it(reports);
			while(it.hasNext())
			{
				it.next();

				const QByteArray &routeId = it.key();
				Report *report = it.value();

				if(report->connectionsMaxStale)
					updateConnectionsMax(routeId);

				StatsPacket p;
				p.type = StatsPacket::Report;
				p.from = instanceId;
				p.route = routeId;
				p.connectionsMax = report->connectionsMax;
				p.connectionsMinutes = report->connectionsMinutes;
				p.messagesReceived = report->messagesReceived;
				p.messagesSent = report->messagesSent;
				p.httpResponseMessagesSent = report->httpResponseMessagesSent;

				report->connectionsMaxStale = true;
				report->connectionsMinutes = 0;
				report->messagesReceived = 0;
				report->messagesSent = 0;
				report->httpResponseMessagesSent = 0;

				if(report->isEmpty())
				{
					// if report is empty, we can throw it out after sending
					toDelete += report;
				}

				if(sock)
					write(p);

				reportPackets += p;
			}

			foreach(Report *report, toDelete)
			{
				removeReport(report);
				delete report;
			}

			QPointer<QObject> self = this;
			foreach(const StatsPacket &p, reportPackets)
			{
				emit q->reported(p);
				if(!self)
					return;
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

void StatsManager::setReportsEnabled(bool on)
{
	d->reportsEnabled = on;
	d->setupReports();
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

void StatsManager::addMessage(const QString &channel, const QString &itemId, const QString &transport, int count)
{
	d->sendMessage(channel, itemId, transport, count);
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
		d->removeConnection(c);
		delete c;
	}

	QDateTime now = QDateTime::currentDateTimeUtc();

	c = new Private::ConnectionInfo;
	c->id = id;
	c->routeId = routeId;
	c->type = type;
	c->peerAddress = peerAddress;
	c->ssl = ssl;
	c->lastReported = now; // start counting minutes from current time
	d->insertConnection(c);

	if(d->reportsEnabled)
	{
		// check if this connection should replace a lingering external one
		bool replacing = false;
		QHashIterator<QByteArray, QHash<QByteArray, Private::ConnectionInfo*> > it(d->externalConnectionInfoByFrom);
		while(it.hasNext())
		{
			it.next();
			const QHash<QByteArray, Private::ConnectionInfo*> &extConnectionInfoById = it.value();

			Private::ConnectionInfo *other = extConnectionInfoById.value(c->id);
			if(other)
			{
				replacing = true;
				d->removeExternalConnection(other);
				break;
			}
		}

		d->updateConnectionsMax(c->routeId);

		if(!replacing)
		{
			Private::Report *report = d->getOrCreateReport(c->routeId);
			++(report->connectionsMinutes); // minutes are rounded up so count one immediately
		}
	}

	c->nextRefresh = now.addSecs(CONNECTION_REFRESH);

	if(!quiet)
		d->sendConnected(c);
}

void StatsManager::removeConnection(const QByteArray &id, bool linger)
{
	Private::ConnectionInfo *c = d->connectionInfoById.value(id);
	if(!c)
		return;

	QDateTime now = QDateTime::currentDateTimeUtc();
	QByteArray routeId = c->routeId;

	if(d->reportsEnabled)
		d->updateConnectionsMinutes(c, now);

	if(linger)
	{
		if(!c->linger)
		{
			c->linger = true;
			c->nextRefresh = now.addSecs(CONNECTION_LINGER);
		}
	}
	else
	{
		d->sendDisconnected(c);
		d->removeConnection(c);
		delete c;
	}

	if(d->reportsEnabled)
		d->updateConnectionsMax(routeId);
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

		s->nextRefresh = QDateTime::currentDateTimeUtc().addSecs(SUBSCRIPTION_REFRESH);
		d->sendSubscribed(s);
	}
	else
	{
		if(s->linger)
		{
			// if this was a lingering subscription, return it to normal
			s->linger = false;
			s->nextRefresh = QDateTime::currentDateTimeUtc().addSecs(SUBSCRIPTION_REFRESH);
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
			s->nextRefresh = QDateTime::currentDateTimeUtc().addSecs(SUBSCRIPTION_LINGER);
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

void StatsManager::addMessageReceived(const QByteArray &routeId)
{
	assert(d->reportsEnabled);

	Private::Report *report = d->getOrCreateReport(routeId);

	++report->messagesReceived;

	report->lastUpdated = QDateTime::currentDateTimeUtc();
}

void StatsManager::addMessageSent(const QByteArray &routeId, const QString &transport)
{
	assert(d->reportsEnabled);

	Private::Report *report = d->getOrCreateReport(routeId);

	++report->messagesSent;

	if(transport == "http-response")
		++report->httpResponseMessagesSent;

	report->lastUpdated = QDateTime::currentDateTimeUtc();
}

bool StatsManager::checkConnection(const QByteArray &id)
{
	return d->connectionInfoById.contains(id);
}

void StatsManager::processExternalPacket(const StatsPacket &packet)
{
	assert(d->reportsEnabled);

	if(packet.type != StatsPacket::Connected && packet.type != StatsPacket::Disconnected)
		return;

	QDateTime now = QDateTime::currentDateTimeUtc();

	QHash<QByteArray, Private::ConnectionInfo*> &extConnectionInfoById = d->externalConnectionInfoByFrom[packet.from];

	if(packet.type == StatsPacket::Connected)
	{
		// add/update
		Private::ConnectionInfo *c = extConnectionInfoById.value(packet.connectionId);
		if(!c)
		{
			c = new Private::ConnectionInfo;
			c->id = packet.connectionId;
			c->routeId = packet.route;
			c->type = packet.connectionType == StatsPacket::Http ? Http : WebSocket;
			c->peerAddress = packet.peerAddress;
			c->ssl = packet.ssl;
			c->lastReported = now;
			c->from = packet.from;
			d->insertExternalConnection(c);

			d->updateConnectionsMax(c->routeId);

			Private::Report *report = d->getOrCreateReport(c->routeId);
			++(report->connectionsMinutes); // minutes are rounded up so count one immediately
		}

		c->ttl = packet.ttl;
		c->lastUpdated = now;
	}
	else // Disconnected
	{
		Private::ConnectionInfo *c = extConnectionInfoById.value(packet.connectionId);
		if(!c)
			return;

		QByteArray routeId = c->routeId;

		d->updateConnectionsMinutes(c, now);
		d->removeExternalConnection(c);
		delete c;

		d->updateConnectionsMax(routeId);
	}
}

void StatsManager::sendPacket(const StatsPacket &packet)
{
	if(!d->sock)
		return;

	StatsPacket p = packet;
	p.from = d->instanceId;
	d->write(p);
}

#include "statsmanager.moc"
