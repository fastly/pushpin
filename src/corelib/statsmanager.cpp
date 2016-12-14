/*
 * Copyright (C) 2014-2016 Fanout, Inc.
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
#include <QDateTime>
#include <QPointer>
#include <QTimer>
#include "qzmqsocket.h"
#include "log.h"
#include "tnetstring.h"
#include "packet/statspacket.h"
#include "zutil.h"

// make this somewhat big since PUB is lossy
#define OUT_HWM 200000

#define ACTIVITY_TIMEOUT 100
#define REPORT_INTERVAL (10 * 1000)

#define REFRESH_INTERVAL 1000
#define CONNECTION_EXPIRE (120 * 1000)
#define CONNECTION_LINGER (60 * 1000)
#define SUBSCRIPTION_EXPIRE (60 * 1000)
#define SUBSCRIPTION_LINGER (60 * 1000)

#define CONNECTION_SHOULD_PROCESS (CONNECTION_EXPIRE * 3 / 4)
#define CONNECTION_MUST_PROCESS (CONNECTION_EXPIRE * 4 / 5)
#define CONNECTION_REFRESH_BUCKETS (CONNECTION_SHOULD_PROCESS / REFRESH_INTERVAL)

#define SUBSCRIPTION_SHOULD_PROCESS (SUBSCRIPTION_EXPIRE * 3 / 4)
#define SUBSCRIPTION_MUST_PROCESS (SUBSCRIPTION_EXPIRE * 4 / 5)
#define SUBSCRIPTION_REFRESH_BUCKETS (SUBSCRIPTION_SHOULD_PROCESS / REFRESH_INTERVAL)

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
		qint64 lastRefresh;
		int refreshBucket;
		bool linger;
		qint64 lastReport;
		QByteArray from; // external
		int ttl; // external
		qint64 lastActive; // external

		ConnectionInfo() :
			ssl(false),
			lastRefresh(-1),
			refreshBucket(-1),
			linger(false),
			lastReport(-1),
			ttl(-1),
			lastActive(-1)
		{
		}
	};

	class Subscription
	{
	public:
		QString mode;
		QString channel;
		qint64 lastRefresh;
		int refreshBucket;
		bool linger;

		Subscription() :
			lastRefresh(-1),
			refreshBucket(-1),
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
		int blocksReceived;
		int blocksSent;
		qint64 lastUpdate;

		Report() :
			connectionsMax(0),
			connectionsMaxStale(true),
			connectionsMinutes(0),
			messagesReceived(0),
			messagesSent(0),
			httpResponseMessagesSent(0),
			blocksReceived(-1),
			blocksSent(-1),
			lastUpdate(-1)
		{
		}

		bool isEmpty() const
		{
			return (connectionsMax == 0 &&
				connectionsMinutes == 0 &&
				messagesReceived == 0 &&
				messagesSent == 0 &&
				httpResponseMessagesSent == 0 &&
				blocksReceived <= 0 &&
				blocksSent <= 0);
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
	QMap<QPair<qint64, ConnectionInfo*>, ConnectionInfo*> connectionInfoByLastRefresh;
	QSet<ConnectionInfo*> connectionInfoRefreshBuckets[CONNECTION_REFRESH_BUCKETS];
	int currentConnectionInfoRefreshBucket;
	QHash<QByteArray, QHash<QByteArray, ConnectionInfo*> > externalConnectionInfoByFrom;
	QHash<QByteArray, QSet<ConnectionInfo*> > externalConnectionInfoByRoute;
	QMap<QPair<qint64, ConnectionInfo*>, ConnectionInfo*> externalConnectionInfoByLastActive;
	QHash<SubscriptionKey, Subscription*> subscriptionsByKey;
	QMap<QPair<qint64, Subscription*>, Subscription*> subscriptionsByLastRefresh;
	QSet<Subscription*> subscriptionRefreshBuckets[SUBSCRIPTION_REFRESH_BUCKETS];
	int currentSubscriptionRefreshBucket;
	QHash<QByteArray, Report*> reports;
	QTimer *activityTimer;
	QTimer *reportTimer;
	QTimer *refreshTimer;
	bool reportsEnabled;

	Private(StatsManager *_q) :
		QObject(_q),
		q(_q),
		ipcFileMode(-1),
		sock(0),
		currentConnectionInfoRefreshBucket(0),
		currentSubscriptionRefreshBucket(0),
		reportTimer(0),
		reportsEnabled(false)
	{
		activityTimer = new QTimer(this);
		connect(activityTimer, &QTimer::timeout, this, &Private::activity_timeout);
		activityTimer->setSingleShot(true);

		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, &Private::refresh_timeout);
		refreshTimer->start(REFRESH_INTERVAL);
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

		if(reportTimer)
		{
			reportTimer->setParent(0);
			reportTimer->disconnect(this);
			reportTimer->deleteLater();
			reportTimer = 0;
		}

		if(refreshTimer)
		{
			refreshTimer->setParent(0);
			refreshTimer->disconnect(this);
			refreshTimer->deleteLater();
			refreshTimer = 0;
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
		sock->setWriteQueueEnabled(false);
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
			reportTimer->start(REPORT_INTERVAL);
		}
		else if(!reportsEnabled && reportTimer)
		{
			reportTimer->setParent(0);
			reportTimer->disconnect(this);
			reportTimer->deleteLater();
			reportTimer = 0;
		}
	}

	int smallestConnectionInfoRefreshBucket()
	{
		int best = -1;
		int bestSize;
		for(int n = 0; n < CONNECTION_REFRESH_BUCKETS; ++n)
		{
			if(best == -1 || connectionInfoRefreshBuckets[n].count() < bestSize)
			{
				best = n;
				bestSize = connectionInfoRefreshBuckets[n].count();
			}
		}

		return best;
	}

	int smallestSubscriptionRefreshBucket()
	{
		int best = -1;
		int bestSize;
		for(int n = 0; n < SUBSCRIPTION_REFRESH_BUCKETS; ++n)
		{
			if(best == -1 || subscriptionRefreshBuckets[n].count() < bestSize)
			{
				best = n;
				bestSize = subscriptionRefreshBuckets[n].count();
			}
		}

		return best;
	}

	void insertConnection(ConnectionInfo *c)
	{
		connectionInfoById[c->id] = c;

		QSet<ConnectionInfo*> &cs = connectionInfoByRoute[c->routeId];
		cs += c;

		assert(c->lastRefresh >= 0);
		connectionInfoByLastRefresh.insert(QPair<qint64, ConnectionInfo*>(c->lastRefresh, c), c);

		c->refreshBucket = smallestConnectionInfoRefreshBucket();
		connectionInfoRefreshBuckets[c->refreshBucket] += c;
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

		if(c->lastRefresh >= 0)
		{
			connectionInfoByLastRefresh.remove(QPair<qint64, ConnectionInfo*>(c->lastRefresh, c));
			connectionInfoRefreshBuckets[c->refreshBucket].remove(c);
		}
	}

	void insertExternalConnection(ConnectionInfo *c)
	{
		QHash<QByteArray, ConnectionInfo*> &extConnectionInfoById = externalConnectionInfoByFrom[c->from];
		extConnectionInfoById[c->id] = c;

		QSet<ConnectionInfo*> &cs = externalConnectionInfoByRoute[c->routeId];
		cs += c;

		assert(c->lastActive >= 0);
		externalConnectionInfoByLastActive.insert(QPair<qint64, ConnectionInfo*>(c->lastActive, c), c);

		assert(c->lastRefresh == -1);
		assert(c->refreshBucket == -1);
	}

	void removeExternalConnection(ConnectionInfo *c)
	{
		assert(externalConnectionInfoByFrom.contains(c->from));

		QHash<QByteArray, ConnectionInfo*> &extConnectionInfoById = externalConnectionInfoByFrom[c->from];
		extConnectionInfoById.remove(c->id);

		if(extConnectionInfoById.isEmpty())
			externalConnectionInfoByFrom.remove(c->from);

		if(externalConnectionInfoByRoute.contains(c->routeId))
		{
			QSet<ConnectionInfo*> &cs = externalConnectionInfoByRoute[c->routeId];
			cs.remove(c);

			if(cs.isEmpty())
				externalConnectionInfoByRoute.remove(c->routeId);
		}

		if(c->lastActive >= 0)
			externalConnectionInfoByLastActive.remove(QPair<qint64, ConnectionInfo*>(c->lastActive, c));
	}

	void insertSubscription(Subscription *s)
	{
		SubscriptionKey subKey(s->mode, s->channel);

		subscriptionsByKey[subKey] = s;

		assert(s->lastRefresh >= 0);
		subscriptionsByLastRefresh.insert(QPair<qint64, Subscription*>(s->lastRefresh, s), s);

		s->refreshBucket = smallestSubscriptionRefreshBucket();
		subscriptionRefreshBuckets[s->refreshBucket] += s;
	}

	void removeSubscription(Subscription *s)
	{
		SubscriptionKey subKey(s->mode, s->channel);

		subscriptionsByKey.remove(subKey);

		if(s->lastRefresh >= 0)
		{
			subscriptionsByLastRefresh.remove(QPair<qint64, Subscription*>(s->lastRefresh, s));
			subscriptionRefreshBuckets[s->refreshBucket].remove(s);
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

	void sendMessage(const QString &channel, const QString &itemId, const QString &transport, int count, int blocks)
	{
		if(!sock)
			return;

		StatsPacket p;
		p.type = StatsPacket::Message;
		p.from = instanceId;
		p.channel = channel.toUtf8();
		p.itemId = itemId.toUtf8();
		p.count = count;
		p.blocks = blocks;
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
		p.ttl = CONNECTION_EXPIRE / 1000;
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
		p.ttl = SUBSCRIPTION_EXPIRE / 1000;
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

	void updateConnectionsMax(const QByteArray &routeId, qint64 now)
	{
		Report *report = getOrCreateReport(routeId);

		int localConns = connectionInfoByRoute.value(routeId).count();
		int extConns = externalConnectionInfoByRoute.value(routeId).count();

		int conns = localConns + extConns;

		if(report->connectionsMaxStale)
		{
			report->connectionsMax = conns;
			report->connectionsMaxStale = false;
		}
		else
			report->connectionsMax = qMax(report->connectionsMax, conns);

		report->lastUpdate = now;
	}

	void updateConnectionsMinutes(ConnectionInfo *c, qint64 now)
	{
		// ignore lingering connections
		if(c->linger)
			return;

		Report *report = getOrCreateReport(c->routeId);

		int mins = (now - c->lastReport) / 60000;
		if(mins > 0)
		{
			// only advance as much as we've read
			c->lastReport += mins * 60000;

			report->connectionsMinutes += mins;

			report->lastUpdate = now;
		}
	}

	void refreshConnections(qint64 now)
	{
		QList<ConnectionInfo*> toRefresh;
		QList<ConnectionInfo*> toDelete;

		// process the current bucket
		const QSet<ConnectionInfo*> &bucket = connectionInfoRefreshBuckets[currentConnectionInfoRefreshBucket];
		foreach(ConnectionInfo *c, bucket)
		{
			// don't bucket-process lingered connections
			if(c->linger)
				continue;

			// move to the end
			QPair<qint64, ConnectionInfo*> k(c->lastRefresh, c);
			connectionInfoByLastRefresh.remove(k);
			c->lastRefresh = now;
			connectionInfoByLastRefresh.insert(QPair<qint64, ConnectionInfo*>(c->lastRefresh, c), c);

			toRefresh += c;
		}

		// process any others
		qint64 threshold = now - CONNECTION_MUST_PROCESS;
		while(!connectionInfoByLastRefresh.isEmpty())
		{
			QMap<QPair<qint64, ConnectionInfo*>, ConnectionInfo*>::iterator it = connectionInfoByLastRefresh.begin();
			ConnectionInfo *c = it.value();

			if(c->lastRefresh > threshold)
				break;

			if(c->linger)
			{
				// in linger mode, next refresh is set to the time we should
				//   delete the connection rather than refresh
				toDelete += c;

				// need to remove from map to avoid reprocessing
				connectionInfoByLastRefresh.erase(it);
				connectionInfoRefreshBuckets[c->refreshBucket].remove(c);
				c->lastRefresh = -1;
			}
			else
			{
				// move to the end
				connectionInfoByLastRefresh.erase(it);
				c->lastRefresh = now;
				connectionInfoByLastRefresh.insert(QPair<qint64, ConnectionInfo*>(c->lastRefresh, c), c);

				toRefresh += c;
			}
		}

		QList<QByteArray> refreshedIds;
		foreach(ConnectionInfo *c, toRefresh)
		{
			refreshedIds += c->id;

			updateConnectionsMinutes(c, now);
			sendConnected(c);
		}

		foreach(ConnectionInfo *c, toDelete)
		{
			// note: we don't send a disconnect message when the
			//   linger expires. the assumption is that the handler
			//   owns the connection now

			removeConnection(c);
			delete c;
		}

		if(!refreshedIds.isEmpty())
			emit q->connectionsRefreshed(refreshedIds);

		++currentConnectionInfoRefreshBucket;
		if(currentConnectionInfoRefreshBucket >= CONNECTION_REFRESH_BUCKETS)
			currentConnectionInfoRefreshBucket = 0;
	}

	void expireExternalConnections(qint64 now)
	{
		// external connections should only be tracked if reporting is enabled
		if(!reportsEnabled)
			return;

		QSet<QByteArray> routesUpdated;

		qint64 threshold = now - CONNECTION_EXPIRE;
		while(!externalConnectionInfoByLastActive.isEmpty())
		{
			QMap<QPair<qint64, ConnectionInfo*>, ConnectionInfo*>::iterator it = externalConnectionInfoByLastActive.begin();
			ConnectionInfo *c = it.value();

			if(c->lastActive > threshold)
				break;

			routesUpdated += c->routeId;
			updateConnectionsMinutes(c, now);
			removeExternalConnection(c);
			delete c;
		}

		foreach(const QByteArray &routeId, routesUpdated)
			updateConnectionsMax(routeId, now);
	}

	void refreshSubscriptions(qint64 now)
	{
		QList<Subscription*> toRefresh;
		QList<Subscription*> toDelete;

		// process the current bucket
		const QSet<Subscription*> &bucket = subscriptionRefreshBuckets[currentSubscriptionRefreshBucket];
		foreach(Subscription *s, bucket)
		{
			// don't bucket-process lingered subscriptions
			if(s->linger)
				continue;

			// move to the end
			QPair<qint64, Subscription*> k(s->lastRefresh, s);
			subscriptionsByLastRefresh.remove(k);
			s->lastRefresh = now;
			subscriptionsByLastRefresh.insert(QPair<qint64, Subscription*>(s->lastRefresh, s), s);

			toRefresh += s;
		}

		// process any others
		qint64 threshold = now - SUBSCRIPTION_MUST_PROCESS;
		while(!subscriptionsByLastRefresh.isEmpty())
		{
			QMap<QPair<qint64, Subscription*>, Subscription*>::iterator it = subscriptionsByLastRefresh.begin();
			Subscription *s = it.value();

			if(s->lastRefresh > threshold)
				break;

			if(s->linger)
			{
				// in linger mode, next refresh is set to the time we should
				//   delete the subscription rather than refresh
				toDelete += s;

				// need to remove from map to avoid reprocessing
				subscriptionsByLastRefresh.erase(it);
				subscriptionRefreshBuckets[s->refreshBucket].remove(s);
				s->lastRefresh = -1;
			}
			else
			{
				// move to the end
				subscriptionsByLastRefresh.erase(it);
				s->lastRefresh = now;
				subscriptionsByLastRefresh.insert(QPair<qint64, Subscription*>(s->lastRefresh, s), s);

				toRefresh += s;
			}
		}

		foreach(Subscription *s, toRefresh)
			sendSubscribed(s);

		foreach(Subscription *s, toDelete)
		{
			QString mode = s->mode;
			QString channel = s->channel;

			sendUnsubscribed(s);
			removeSubscription(s);
			delete s;

			emit q->unsubscribed(mode, channel);
		}

		++currentSubscriptionRefreshBucket;
		if(currentSubscriptionRefreshBucket >= SUBSCRIPTION_REFRESH_BUCKETS)
			currentSubscriptionRefreshBucket = 0;
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

	void report_timeout()
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		QList<StatsPacket> reportPackets;
		QList<Report*> toDelete;

		// note: here we iterate over all reports, which will be one per
		//   route ID. this could become a problem if there are thousands
		//   of route IDs (at which point we can consider bucketing)
		QHashIterator<QByteArray, Report*> it(reports);
		while(it.hasNext())
		{
			it.next();

			const QByteArray &routeId = it.key();
			Report *report = it.value();

			if(report->connectionsMaxStale)
				updateConnectionsMax(routeId, now);

			StatsPacket p;
			p.type = StatsPacket::Report;
			p.from = instanceId;
			p.route = routeId;
			p.connectionsMax = report->connectionsMax;
			p.connectionsMinutes = report->connectionsMinutes;
			p.messagesReceived = report->messagesReceived;
			p.messagesSent = report->messagesSent;
			p.httpResponseMessagesSent = report->httpResponseMessagesSent;
			p.blocksReceived = report->blocksReceived;
			p.blocksSent = report->blocksSent;

			report->connectionsMaxStale = true;
			report->connectionsMinutes = 0;
			report->messagesReceived = 0;
			report->messagesSent = 0;
			report->httpResponseMessagesSent = 0;
			report->blocksReceived = -1;
			report->blocksSent = -1;

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

		if(!reportPackets.isEmpty())
			emit q->reported(reportPackets);
	}

	void refresh_timeout()
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		refreshConnections(now);
		expireExternalConnections(now);
		refreshSubscriptions(now);
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

void StatsManager::addMessage(const QString &channel, const QString &itemId, const QString &transport, int count, int blocks)
{
	d->sendMessage(channel, itemId, transport, count, blocks);
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

	qint64 now = QDateTime::currentMSecsSinceEpoch();

	c = new Private::ConnectionInfo;
	c->id = id;
	c->routeId = routeId;
	c->type = type;
	c->peerAddress = peerAddress;
	c->ssl = ssl;
	c->lastRefresh = now;
	c->lastReport = now; // start counting minutes from current time
	d->insertConnection(c);

	if(d->reportsEnabled)
	{
		// check if this connection should replace a lingering external one
		// note: this iterates over all the known external sources, which at
		//   at the time of this writing is almost certainly just 1 (a single
		//   pushpin-proxy source).
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
				delete other;
				break;
			}
		}

		d->updateConnectionsMax(c->routeId, now);

		// only count a minute if we weren't replacing
		if(!replacing)
		{
			Private::Report *report = d->getOrCreateReport(c->routeId);
			++(report->connectionsMinutes); // minutes are rounded up so count one immediately
		}
	}

	if(!quiet)
		d->sendConnected(c);
}

void StatsManager::removeConnection(const QByteArray &id, bool linger)
{
	Private::ConnectionInfo *c = d->connectionInfoById.value(id);
	if(!c)
		return;

	qint64 now = QDateTime::currentMSecsSinceEpoch();
	QByteArray routeId = c->routeId;

	if(d->reportsEnabled)
		d->updateConnectionsMinutes(c, now);

	if(linger)
	{
		if(!c->linger)
		{
			c->linger = true;

			// hack to ensure full linger time honored by refresh processing
			qint64 lingerStartTime = now + (CONNECTION_EXPIRE - CONNECTION_MUST_PROCESS);

			// move to the end
			d->connectionInfoByLastRefresh.remove(QPair<qint64, Private::ConnectionInfo*>(c->lastRefresh, c));
			c->lastRefresh = lingerStartTime;
			d->connectionInfoByLastRefresh.insert(QPair<qint64, Private::ConnectionInfo*>(c->lastRefresh, c), c);
		}
	}
	else
	{
		d->sendDisconnected(c);
		d->removeConnection(c);
		delete c;
	}

	if(d->reportsEnabled)
		d->updateConnectionsMax(routeId, now);
}

void StatsManager::refreshConnection(const QByteArray &id)
{
	Private::ConnectionInfo *c = d->connectionInfoById.value(id);
	if(!c)
		return;

	d->sendConnected(c);
}

void StatsManager::addSubscription(const QString &mode, const QString &channel)
{
	Private::SubscriptionKey subKey(mode, channel);
	Private::Subscription *s = d->subscriptionsByKey.value(subKey);
	if(!s)
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		// add the subscription if we didn't have it
		s = new Private::Subscription;
		s->mode = mode;
		s->channel = channel;
		s->lastRefresh = now;
		d->insertSubscription(s);

		d->sendSubscribed(s);
	}
	else
	{
		if(s->linger)
		{
			qint64 now = QDateTime::currentMSecsSinceEpoch();

			// if this was a lingering subscription, return it to normal
			s->linger = false;

			// move to the end
			d->subscriptionsByLastRefresh.remove(QPair<qint64, Private::Subscription*>(s->lastRefresh, s));
			s->lastRefresh = now;
			d->subscriptionsByLastRefresh.insert(QPair<qint64, Private::Subscription*>(s->lastRefresh, s), s);

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
			qint64 now = QDateTime::currentMSecsSinceEpoch();

			s->linger = true;

			// hack to ensure full linger time honored by refresh processing
			qint64 lingerStartTime = now + (SUBSCRIPTION_EXPIRE - SUBSCRIPTION_MUST_PROCESS);

			// move to the end
			d->subscriptionsByLastRefresh.remove(QPair<qint64, Private::Subscription*>(s->lastRefresh, s));
			s->lastRefresh = lingerStartTime;
			d->subscriptionsByLastRefresh.insert(QPair<qint64, Private::Subscription*>(s->lastRefresh, s), s);
		}
	}
	else
	{
		d->sendUnsubscribed(s);
		d->removeSubscription(s);
		delete s;

		emit unsubscribed(mode, channel);
	}
}

void StatsManager::addMessageReceived(const QByteArray &routeId, int blocks)
{
	assert(d->reportsEnabled);

	Private::Report *report = d->getOrCreateReport(routeId);

	++report->messagesReceived;

	if(blocks > 0)
	{
		if(report->blocksReceived < 0)
			report->blocksReceived = 0;

		report->blocksReceived += blocks;
	}

	report->lastUpdate = QDateTime::currentMSecsSinceEpoch();
}

void StatsManager::addMessageSent(const QByteArray &routeId, const QString &transport, int blocks)
{
	assert(d->reportsEnabled);

	Private::Report *report = d->getOrCreateReport(routeId);

	++report->messagesSent;

	if(transport == "http-response")
		++report->httpResponseMessagesSent;

	if(blocks > 0)
	{
		if(report->blocksSent < 0)
			report->blocksSent = 0;

		report->blocksSent += blocks;
	}

	report->lastUpdate = QDateTime::currentMSecsSinceEpoch();
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

	// can't add an external connection with same ID as local
	if(packet.type == StatsPacket::Connected && d->connectionInfoById.contains(packet.connectionId))
		return;

	// if the connection exists under a different from address, remove it.
	// note: this iterates over all the known external sources, which at
	//   at the time of this writing is almost certainly just 1 (a single
	//   pushpin-proxy source).
	QList<Private::ConnectionInfo*> toDelete;
	QHashIterator<QByteArray, QHash<QByteArray, Private::ConnectionInfo*> > it(d->externalConnectionInfoByFrom);
	while(it.hasNext())
	{
		it.next();
		const QByteArray &from = it.key();

		if(from == packet.from)
			continue;

		const QHash<QByteArray, Private::ConnectionInfo*> &extConnectionInfoById = it.value();

		Private::ConnectionInfo *c = extConnectionInfoById.value(packet.connectionId);
		if(c)
			toDelete += c;
	}
	foreach(Private::ConnectionInfo *c, toDelete)
	{
		d->removeExternalConnection(c);
		delete c;
	}

	qint64 now = QDateTime::currentMSecsSinceEpoch();

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
			c->lastReport = now;
			c->from = packet.from;
			c->lastActive = now;
			d->insertExternalConnection(c);

			d->updateConnectionsMax(c->routeId, now);

			Private::Report *report = d->getOrCreateReport(c->routeId);
			++(report->connectionsMinutes); // minutes are rounded up so count one immediately
		}
		else
		{
			c->ttl = packet.ttl;

			// move to the end
			d->externalConnectionInfoByLastActive.remove(QPair<qint64, Private::ConnectionInfo*>(c->lastActive, c));
			c->lastActive = now;
			d->externalConnectionInfoByLastActive.insert(QPair<qint64, Private::ConnectionInfo*>(c->lastActive, c), c);
		}

		d->updateConnectionsMinutes(c, now);
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

		d->updateConnectionsMax(routeId, now);
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
