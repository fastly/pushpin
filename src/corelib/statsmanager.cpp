/*
 * Copyright (C) 2014-2022 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
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
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#include "statsmanager.h"

#include <assert.h>
#include <QVector>
#include <QDateTime>
#include <QPointer>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include "qzmqsocket.h"
#include "timerwheel.h"
#include "log.h"
#include "tnetstring.h"
#include "httpheaders.h"
#include "packet/statspacket.h"
#include "simplehttpserver.h"
#include "zutil.h"

// make this somewhat big since PUB is lossy
#define OUT_HWM 200000

#define ACTIVITY_TIMEOUT 100
#define REFRESH_INTERVAL 1000

#define SHOULD_PROCESS_TIME(x) (x * 3 / 4)

#define TICK_DURATION_MS 10

static qint64 durationToTicksRoundDown(qint64 msec)
{
	return msec / TICK_DURATION_MS;
}

static qint64 durationToTicksRoundUp(qint64 msec)
{
	return (msec + TICK_DURATION_MS - 1) / TICK_DURATION_MS;
}

class StatsManager::Private : public QObject
{
	Q_OBJECT

public:
	class TimerBase
	{
	public:
		enum Type
		{
			Connection,
			ExternalConnection,
			Subscription,
		};

		Type timerType;
		int timerId;

		TimerBase() :
			timerId(-1)
		{
		}
	};

	class ConnectionInfo : public TimerBase
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

	class Subscription : public TimerBase
	{
	public:
		QString mode;
		QString channel;
		int subscriberCount;
		qint64 lastRefresh;
		int refreshBucket;
		bool linger;

		Subscription() :
			subscriberCount(0),
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
		int requestsReceived;
		qint64 lastUpdate;
		qint64 startTime;

		Report() :
			connectionsMax(0),
			connectionsMaxStale(true),
			connectionsMinutes(0),
			messagesReceived(0),
			messagesSent(0),
			httpResponseMessagesSent(0),
			blocksReceived(-1),
			blocksSent(-1),
			requestsReceived(0),
			lastUpdate(-1),
			startTime(-1)
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
				blocksSent <= 0 &&
				requestsReceived == 0);
		}

		void addConnectionsMinutes(int mins, qint64 now)
		{
			connectionsMinutes += mins;

			lastUpdate = now;
		}

		void addMessageReceived(int blocks, qint64 now)
		{
			++messagesReceived;

			if(blocks > 0)
			{
				if(blocksReceived < 0)
					blocksReceived = 0;

				blocksReceived += blocks;
			}

			lastUpdate = now;
		}

		void addMessageSent(const QString &transport, int blocks, qint64 now)
		{
			++messagesSent;

			if(transport == "http-response")
				++httpResponseMessagesSent;

			if(blocks > 0)
			{
				if(blocksSent < 0)
					blocksSent = 0;

				blocksSent += blocks;
			}

			lastUpdate = now;
		}

		void addRequestsReceived(int count, qint64 now)
		{
			requestsReceived += count;

			lastUpdate = now;
		}
	};

	class Counts
	{
	public:
		int requestsReceived;

		Counts() :
			requestsReceived(0)
		{
		}

		bool isEmpty()
		{
			return (requestsReceived == 0);
		}
	};

	class PrometheusMetric
	{
	public:
		enum Type
		{
			RequestReceived,
			ConnectionConnected,
			ConnectionMinute,
			MessageReceived,
			MessageSent
		};

		Type mtype;
		QString name;
		QString type;
		QString help;

		PrometheusMetric(Type _mtype, const QString &_name, const QString &_type, const QString &_help) :
			mtype(_mtype),
			name(_name),
			type(_type),
			help(_help)
		{
		}
	};

	typedef QPair<QString, QString> SubscriptionKey;

	StatsManager *q;
	int connectionsMax;
	int subscriptionsMax;
	QByteArray instanceId;
	int ipcFileMode;
	QString spec;
	Format outputFormat;
	int connectionTtl;
	int connectionLinger;
	int subscriptionTtl;
	int subscriptionLinger;
	int reportInterval;
	QZmq::Socket *sock;
	SimpleHttpServer *prometheusServer;
	QString prometheusPrefix;
	QList<PrometheusMetric> prometheusMetrics;
	QHash<QByteArray, int> routeActivity;
	QHash<QByteArray, ConnectionInfo*> connectionInfoById;
	QHash<QByteArray, QSet<ConnectionInfo*> > connectionInfoByRoute;
	QVector<QSet<ConnectionInfo*> > connectionInfoRefreshBuckets;
	int currentConnectionInfoRefreshBucket;
	QHash<QByteArray, QHash<QByteArray, ConnectionInfo*> > externalConnectionInfoByFrom;
	QHash<QByteArray, QSet<ConnectionInfo*> > externalConnectionInfoByRoute;
	QHash<SubscriptionKey, Subscription*> subscriptionsByKey;
	QVector<QSet<Subscription*> > subscriptionRefreshBuckets;
	int currentSubscriptionRefreshBucket;
	TimerWheel wheel;
	qint64 startTime;
	QHash<QByteArray, Report*> reports;
	Counts combinedCounts;
	Report combinedReport;
	QTimer *activityTimer;
	QTimer *reportTimer;
	QTimer *refreshTimer;

	Private(StatsManager *_q, int _connectionsMax, int _subscriptionsMax) :
		QObject(_q),
		q(_q),
		connectionsMax(_connectionsMax),
		subscriptionsMax(_subscriptionsMax),
		ipcFileMode(-1),
		outputFormat(TnetStringFormat),
		connectionTtl(120 * 1000),
		connectionLinger(60 * 1000),
		subscriptionTtl(60 * 1000),
		subscriptionLinger(60 * 1000),
		reportInterval(10 * 1000),
		sock(0),
		prometheusServer(0),
		currentConnectionInfoRefreshBucket(0),
		currentSubscriptionRefreshBucket(0),
		wheel(TimerWheel((_connectionsMax * 2) + _subscriptionsMax)),
		reportTimer(0)
	{
		activityTimer = new QTimer(this);
		connect(activityTimer, &QTimer::timeout, this, &Private::activity_timeout);
		activityTimer->setSingleShot(true);

		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, &Private::refresh_timeout);
		refreshTimer->start(REFRESH_INTERVAL);

		setupConnectionBuckets();
		setupSubscriptionBuckets();

		prometheusMetrics += PrometheusMetric(PrometheusMetric::RequestReceived, "request_received", "counter", "Number of requests received");
		prometheusMetrics += PrometheusMetric(PrometheusMetric::ConnectionConnected, "connection_connected", "gauge", "Number of concurrent connections");
		prometheusMetrics += PrometheusMetric(PrometheusMetric::ConnectionMinute, "connection_minute", "counter", "Number of minutes clients have been connected");
		prometheusMetrics += PrometheusMetric(PrometheusMetric::MessageReceived, "message_received", "counter", "Number of messages received by the publish API");
		prometheusMetrics += PrometheusMetric(PrometheusMetric::MessageSent,"message_sent", "counter", "Number of messages sent to clients");

		startTime = QDateTime::currentMSecsSinceEpoch();
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

	bool setupSock()
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

	bool setPrometheusPort(const QString &portStr)
	{
		prometheusServer = new SimpleHttpServer(8192, 8192, this);
		connect(prometheusServer, &SimpleHttpServer::requestReady, this, &Private::prometheus_requestReady);

		if(portStr.startsWith("ipc://"))
		{
			if(!prometheusServer->listenLocal(portStr.mid(6)))
			{
				delete prometheusServer;

				return false;
			}
		}
		else
		{
			QHostAddress addr;
			int port = -1;

			int pos = portStr.indexOf(':');
			if(pos >= 0)
			{
				addr = QHostAddress(portStr.mid(0, pos));
				port = portStr.mid(pos + 1).toInt();
			}
			else
			{
				port = portStr.toInt();
			}

			if(!prometheusServer->listen(addr, port))
			{
				delete prometheusServer;

				return false;
			}
		}

		setupReportTimer();

		return true;
	}

	void setupConnectionBuckets()
	{
		int shouldProcessTime = SHOULD_PROCESS_TIME(connectionTtl);
		QVector<QSet<ConnectionInfo*> > newBuckets(qMax(shouldProcessTime / REFRESH_INTERVAL, 1));

		// rebalance (NOTE: this algorithm is not optimal)
		int nextBucketIndex = 0;
		for(int n = 0; n < connectionInfoRefreshBuckets.count(); ++n)
		{
			foreach(ConnectionInfo *c, connectionInfoRefreshBuckets[n])
			{
				newBuckets[nextBucketIndex++] += c;
				if(nextBucketIndex >= newBuckets.count())
					nextBucketIndex = 0;
			}
		}

		connectionInfoRefreshBuckets = newBuckets;
		currentConnectionInfoRefreshBucket = 0;
	}

	void setupSubscriptionBuckets()
	{
		int shouldProcessTime = SHOULD_PROCESS_TIME(subscriptionTtl);
		QVector<QSet<Subscription*> > newBuckets(qMax(shouldProcessTime / REFRESH_INTERVAL, 1));

		// rebalance (NOTE: this algorithm is not optimal)
		int nextBucketIndex = 0;
		for(int n = 0; n < subscriptionRefreshBuckets.count(); ++n)
		{
			foreach(Subscription *s, subscriptionRefreshBuckets[n])
			{
				newBuckets[nextBucketIndex++] += s;
				if(nextBucketIndex >= newBuckets.count())
					nextBucketIndex = 0;
			}
		}

		subscriptionRefreshBuckets = newBuckets;
		currentSubscriptionRefreshBucket = 0;
	}

	void setupReportTimer()
	{
		if(reportInterval > 0 && !reportTimer)
		{
			reportTimer = new QTimer(this);
			connect(reportTimer, &QTimer::timeout, this, &Private::report_timeout);
			reportTimer->start(reportInterval);
		}
		else if(reportInterval <= 0 && reportTimer)
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
		int bestSize = 0;

		for(int n = 0; n < connectionInfoRefreshBuckets.count(); ++n)
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
		int bestSize = 0;

		for(int n = 0; n < subscriptionRefreshBuckets.count(); ++n)
		{
			if(best == -1 || subscriptionRefreshBuckets[n].count() < bestSize)
			{
				best = n;
				bestSize = subscriptionRefreshBuckets[n].count();
			}
		}

		return best;
	}

	void wheelAdd(qint64 timeoutTime, TimerBase *obj)
	{
		if(timeoutTime < startTime)
		{
			timeoutTime = startTime;
		}

		if(obj->timerId >= 0)
		{
			wheel.remove(obj->timerId);
		}

		int id = wheel.add(durationToTicksRoundUp(timeoutTime - startTime), (size_t)obj);
		assert(id >= 0);

		obj->timerId = id;
	}

	void wheelRemove(TimerBase *obj)
	{
		if(obj->timerId >= 0)
		{
			wheel.remove(obj->timerId);
		}

		obj->timerId = -1;
	}

	void insertConnection(ConnectionInfo *c)
	{
		connectionInfoById[c->id] = c;

		QSet<ConnectionInfo*> &cs = connectionInfoByRoute[c->routeId];
		cs += c;

		assert(c->lastRefresh >= 0);
		wheelAdd(c->lastRefresh + SHOULD_PROCESS_TIME(connectionTtl), c);

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
			wheelRemove(c);
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
		wheelAdd(c->lastActive + connectionTtl, c);

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

		wheelRemove(c);
	}

	void insertSubscription(Subscription *s)
	{
		SubscriptionKey subKey(s->mode, s->channel);

		subscriptionsByKey[subKey] = s;

		assert(s->lastRefresh >= 0);
		wheelAdd(s->lastRefresh + SHOULD_PROCESS_TIME(subscriptionTtl), s);

		s->refreshBucket = smallestSubscriptionRefreshBucket();
		subscriptionRefreshBuckets[s->refreshBucket] += s;
	}

	void removeSubscription(Subscription *s)
	{
		SubscriptionKey subKey(s->mode, s->channel);

		subscriptionsByKey.remove(subKey);

		if(s->lastRefresh >= 0)
		{
			wheelRemove(s);
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
			report->startTime = QDateTime::currentMSecsSinceEpoch();
			reports[routeId] = report;
		}

		return report;
	}

	void removeReport(Report *report)
	{
		// subtract the current total from the combined report
		combinedReport.connectionsMax -= report->connectionsMax;

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
		else if(packet.type == StatsPacket::Report)
			prefix = "report";
		else // Counts
			prefix = "counts";

		QVariant vpacket = packet.toVariant();

		QByteArray buf;
		if(outputFormat == TnetStringFormat)
		{
			buf = prefix + " T" + TnetString::fromVariant(vpacket);
		}
		else if(outputFormat == JsonFormat)
		{
			QJsonObject obj = QJsonObject::fromVariantMap(vpacket.toMap());
			buf = prefix + " J" + QJsonDocument(obj).toJson(QJsonDocument::Compact);
		}

		if(!buf.isEmpty())
		{
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				log_debug("stats: OUT %s %s", prefix.data(), qPrintable(TnetString::variantToString(vpacket, -1)));

			sock->write(QList<QByteArray>() << buf);
		}
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
		p.ttl = connectionTtl / 1000;
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
		p.ttl = subscriptionTtl / 1000;
		p.subscribers = s->subscriberCount;
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

	void sendCounts(const Counts &counts)
	{
		if(!sock)
			return;

		StatsPacket p;
		p.type = StatsPacket::Counts;
		p.from = instanceId;
		p.requestsReceived = counts.requestsReceived;
		write(p);
	}

	void updateConnectionsMax(const QByteArray &routeId, qint64 now)
	{
		Report *report = getOrCreateReport(routeId);

		int localConns = connectionInfoByRoute.value(routeId).count();
		int extConns = externalConnectionInfoByRoute.value(routeId).count();

		int conns = localConns + extConns;

		// subtract the current total from the combined report
		combinedReport.connectionsMax -= report->connectionsMax;

		// update the individual report
		if(report->connectionsMaxStale)
		{
			report->connectionsMax = conns;
			report->connectionsMaxStale = false;
		}
		else
			report->connectionsMax = qMax(report->connectionsMax, conns);

		report->lastUpdate = now;

		// add the new total to the combined report
		combinedReport.connectionsMax += report->connectionsMax;
		combinedReport.lastUpdate = now;
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

			report->addConnectionsMinutes(mins, now);
			combinedReport.addConnectionsMinutes(mins, now);
		}
	}

	void handleExpirations(qint64 now)
	{
		QList<QByteArray> refreshedConnIds;
		QSet<QByteArray> routesUpdated;

		while(true)
		{
			TimerWheel::Expired expired = wheel.takeExpired();

			if(expired.key < 0)
			{
				break;
			}

			TimerBase *obj = (TimerBase *)expired.userData;

			obj->timerId = -1;

			switch(obj->timerType)
			{
				case TimerBase::Type::Connection:
				{
					ConnectionInfo *c = static_cast<ConnectionInfo*>(obj);

					if(c->linger)
					{
						// in linger mode, next refresh is set to the time we should
						//   delete the connection rather than refresh

						connectionInfoRefreshBuckets[c->refreshBucket].remove(c);
						c->lastRefresh = -1;

						// note: we don't send a disconnect message when the
						//   linger expires. the assumption is that the handler
						//   owns the connection now

						removeConnection(c);
						delete c;
					}
					else
					{
						c->lastRefresh = now;
						wheelAdd(c->lastRefresh + SHOULD_PROCESS_TIME(connectionTtl), c);

						refreshedConnIds += c->id;

						updateConnectionsMinutes(c, now);
						sendConnected(c);
					}

					break;
				}
				case TimerBase::Type::ExternalConnection:
				{
					ConnectionInfo *c = static_cast<ConnectionInfo*>(obj);

					routesUpdated += c->routeId;
					updateConnectionsMinutes(c, now);
					removeExternalConnection(c);
					delete c;

					break;
				}
				case TimerBase::Type::Subscription:
				{
					Subscription *s = static_cast<Subscription*>(obj);

					if(s->linger)
					{
						// in linger mode, next refresh is set to the time we should
						//   delete the subscription rather than refresh

						subscriptionRefreshBuckets[s->refreshBucket].remove(s);
						s->lastRefresh = -1;

						QString mode = s->mode;
						QString channel = s->channel;

						sendUnsubscribed(s);
						removeSubscription(s);
						delete s;

						emit q->unsubscribed(mode, channel);
					}
					else
					{
						s->lastRefresh = now;
						wheelAdd(s->lastRefresh + SHOULD_PROCESS_TIME(subscriptionTtl), s);

						sendSubscribed(s);
					}

					break;
				}
			}
		}

		if(!refreshedConnIds.isEmpty())
			emit q->connectionsRefreshed(refreshedConnIds);

		foreach(const QByteArray &routeId, routesUpdated)
			updateConnectionsMax(routeId, now);
	}

	void refreshConnections(qint64 now)
	{
		QList<QByteArray> refreshedIds;

		// process the current bucket
		const QSet<ConnectionInfo*> &bucket = connectionInfoRefreshBuckets[currentConnectionInfoRefreshBucket];
		foreach(ConnectionInfo *c, bucket)
		{
			// don't bucket-process lingered connections
			if(c->linger)
				continue;

			c->lastRefresh = now;
			wheelAdd(c->lastRefresh + SHOULD_PROCESS_TIME(connectionTtl), c);

			refreshedIds += c->id;

			updateConnectionsMinutes(c, now);
			sendConnected(c);
		}

		if(!refreshedIds.isEmpty())
			emit q->connectionsRefreshed(refreshedIds);

		++currentConnectionInfoRefreshBucket;
		if(currentConnectionInfoRefreshBucket >= connectionInfoRefreshBuckets.count())
			currentConnectionInfoRefreshBucket = 0;
	}

	void refreshSubscriptions(qint64 now)
	{
		// process the current bucket
		const QSet<Subscription*> &bucket = subscriptionRefreshBuckets[currentSubscriptionRefreshBucket];
		foreach(Subscription *s, bucket)
		{
			// don't bucket-process lingered subscriptions
			if(s->linger)
				continue;

			s->lastRefresh = now;
			wheelAdd(s->lastRefresh + SHOULD_PROCESS_TIME(subscriptionTtl), s);

			sendSubscribed(s);
		}

		++currentSubscriptionRefreshBucket;
		if(currentSubscriptionRefreshBucket >= subscriptionRefreshBuckets.count())
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

		if(!combinedCounts.isEmpty())
		{
			sendCounts(combinedCounts);

			combinedCounts = Counts();
		}
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
			p.duration = now - report->startTime;

			report->startTime = now;
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
		qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

		// time must go forward
		if(currentTime > startTime)
		{
			quint64 currentTicks = (quint64)durationToTicksRoundDown(currentTime - startTime);

			wheel.update(currentTicks);
		}

		handleExpirations(currentTime);

		refreshConnections(currentTime);
		refreshSubscriptions(currentTime);
	}

	void prometheus_requestReady()
	{
		SimpleHttpRequest *req = prometheusServer->takeNext();

		QString data;

		foreach(const PrometheusMetric &m, prometheusMetrics)
		{
			QVariant value;

			switch(m.mtype)
			{
				case PrometheusMetric::RequestReceived: value = QVariant(combinedReport.requestsReceived); break;
				case PrometheusMetric::ConnectionConnected: value = QVariant(combinedReport.connectionsMax); break;
				case PrometheusMetric::ConnectionMinute: value = QVariant(combinedReport.connectionsMinutes); break;
				case PrometheusMetric::MessageReceived: value = QVariant(combinedReport.messagesReceived); break;
				case PrometheusMetric::MessageSent: value = QVariant(combinedReport.messagesSent); break;
			}

			if(value.isNull())
				continue;

			data += QString(
			"# HELP %1%2 %3\n"
			"# TYPE %4%5 %6\n"
			"%7%8 %9\n"
			).arg(prometheusPrefix, m.name, m.help, prometheusPrefix, m.name, m.type, prometheusPrefix, m.name, value.toString());
		}

		connect(req, &SimpleHttpRequest::finished, req, &QObject::deleteLater);

		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		req->respond(200, "OK", headers, data.toUtf8());
	}
};

StatsManager::StatsManager(int connectionsMax, int subscriptionsMax, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, connectionsMax, subscriptionsMax);
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
	return d->setupSock();
}

void StatsManager::setConnectionTtl(int secs)
{
	d->connectionTtl = secs * 1000;
	d->setupConnectionBuckets();
}

void StatsManager::setSubscriptionTtl(int secs)
{
	d->subscriptionTtl = secs * 1000;
	d->setupSubscriptionBuckets();
}

void StatsManager::setSubscriptionLinger(int secs)
{
	d->subscriptionLinger = secs * 1000;
}

void StatsManager::setReportInterval(int secs)
{
	d->reportInterval = secs * 1000;
	d->setupReportTimer();
}

void StatsManager::setOutputFormat(Format format)
{
	d->outputFormat = format;
}

bool StatsManager::setPrometheusPort(const QString &port)
{
	return d->setPrometheusPort(port);
}

void StatsManager::setPrometheusPrefix(const QString &prefix)
{
	d->prometheusPrefix = prefix;
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
	qint64 now = QDateTime::currentMSecsSinceEpoch();

	bool replacing = false;
	qint64 lastReport = now;

	if(d->reportInterval > 0)
	{
		// check if this connection should replace a lingering external one
		// note: this iterates over all the known external sources, which at
		//   at the time of this writing is almost certainly just 1 (a single
		//   pushpin-proxy source).
		QHashIterator<QByteArray, QHash<QByteArray, Private::ConnectionInfo*> > it(d->externalConnectionInfoByFrom);
		while(it.hasNext())
		{
			it.next();
			const QHash<QByteArray, Private::ConnectionInfo*> &extConnectionInfoById = it.value();

			Private::ConnectionInfo *other = extConnectionInfoById.value(id);
			if(other)
			{
				replacing = true;
				lastReport = other->lastReport;

				d->removeExternalConnection(other);
				delete other;

				break;
			}
		}
	}

	// if we already had an entry, silently overwrite it. this can
	//   happen if we sent an accepted connection off to the handler,
	//   kept it lingering in our table, and then the handler passed
	//   it back to us for retrying
	Private::ConnectionInfo *c = d->connectionInfoById.value(id);
	if(c)
	{
		replacing = true;
		lastReport = c->lastReport;

		d->removeConnection(c);
		delete c;
	}

	c = new Private::ConnectionInfo;
	c->timerType = Private::TimerBase::Type::Connection;
	c->id = id;
	c->routeId = routeId;
	c->type = type;
	c->peerAddress = peerAddress;
	c->ssl = ssl;
	c->lastRefresh = now;
	c->lastReport = lastReport;
	d->insertConnection(c);

	if(d->reportInterval > 0)
	{
		d->updateConnectionsMax(c->routeId, now);

		// only count a minute if we weren't replacing
		if(!replacing)
		{
			Private::Report *report = d->getOrCreateReport(c->routeId);

			// minutes are rounded up so count one immediately
			report->addConnectionsMinutes(1, now);
			d->combinedReport.addConnectionsMinutes(1, now);
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

	if(d->reportInterval > 0)
		d->updateConnectionsMinutes(c, now);

	if(linger)
	{
		if(!c->linger)
		{
			c->linger = true;

			// hack to ensure full linger time honored by refresh processing
			qint64 lingerStartTime = now + (d->connectionLinger - SHOULD_PROCESS_TIME(d->connectionTtl));

			c->lastRefresh = lingerStartTime;
			d->wheelAdd(c->lastRefresh + SHOULD_PROCESS_TIME(d->connectionTtl), c);
		}
	}
	else
	{
		d->sendDisconnected(c);
		d->removeConnection(c);
		delete c;
	}

	if(d->reportInterval > 0)
		d->updateConnectionsMax(routeId, now);
}

void StatsManager::refreshConnection(const QByteArray &id)
{
	Private::ConnectionInfo *c = d->connectionInfoById.value(id);
	if(!c)
		return;

	d->sendConnected(c);
}

void StatsManager::addSubscription(const QString &mode, const QString &channel, int subscriberCount)
{
	Private::SubscriptionKey subKey(mode, channel);
	Private::Subscription *s = d->subscriptionsByKey.value(subKey);
	if(!s)
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		// add the subscription if we didn't have it
		s = new Private::Subscription;
		s->timerType = Private::TimerBase::Type::Subscription;
		s->mode = mode;
		s->channel = channel;
		s->subscriberCount = subscriberCount;
		s->lastRefresh = now;
		d->insertSubscription(s);

		d->sendSubscribed(s);
	}
	else
	{
		int oldSubscriberCount = s->subscriberCount;

		s->subscriberCount = subscriberCount;

		if(s->linger)
		{
			qint64 now = QDateTime::currentMSecsSinceEpoch();

			// if this was a lingering subscription, return it to normal
			s->linger = false;

			s->lastRefresh = now;
			d->wheelAdd(s->lastRefresh + SHOULD_PROCESS_TIME(d->subscriptionTtl), s);

			d->sendSubscribed(s);
		}
		else if(s->subscriberCount != oldSubscriberCount)
		{
			qint64 now = QDateTime::currentMSecsSinceEpoch();

			// process soon
			s->lastRefresh = now - SHOULD_PROCESS_TIME(d->subscriptionTtl);
			d->wheelAdd(s->lastRefresh + SHOULD_PROCESS_TIME(d->subscriptionTtl), s);
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
			qint64 lingerStartTime = now + (d->subscriptionLinger - SHOULD_PROCESS_TIME(d->subscriptionTtl));

			s->lastRefresh = lingerStartTime;
			d->wheelAdd(s->lastRefresh + SHOULD_PROCESS_TIME(d->subscriptionTtl), s);
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
	if(d->reportInterval <= 0)
		return;

	Private::Report *report = d->getOrCreateReport(routeId);

	qint64 now = QDateTime::currentMSecsSinceEpoch();

	report->addMessageReceived(blocks, now);
	d->combinedReport.addMessageReceived(blocks, now);
}

void StatsManager::addMessageSent(const QByteArray &routeId, const QString &transport, int blocks)
{
	if(d->reportInterval <= 0)
		return;

	Private::Report *report = d->getOrCreateReport(routeId);

	qint64 now = QDateTime::currentMSecsSinceEpoch();

	report->addMessageSent(transport, blocks, now);
	d->combinedReport.addMessageSent(transport, blocks, now);
}

void StatsManager::addRequestsReceived(int count)
{
	assert(count >= 0);

	qint64 now = QDateTime::currentMSecsSinceEpoch();

	d->combinedCounts.requestsReceived += count;
	d->combinedReport.addRequestsReceived(count, now);

	if(!d->activityTimer->isActive())
		d->activityTimer->start(ACTIVITY_TIMEOUT);
}

bool StatsManager::checkConnection(const QByteArray &id) const
{
	return d->connectionInfoById.contains(id);
}

bool StatsManager::processExternalPacket(const StatsPacket &packet)
{
	if(d->reportInterval <= 0)
		return false;

	if(packet.type != StatsPacket::Connected && packet.type != StatsPacket::Disconnected)
		return false;

	qint64 now = QDateTime::currentMSecsSinceEpoch();

	bool replacing = false;
	qint64 lastReport = now;

	if(packet.type == StatsPacket::Connected)
	{
		// is there a local connection with the same ID?
		Private::ConnectionInfo *c = d->connectionInfoById.value(packet.connectionId);
		if(c)
		{
			// if there is a non-lingering local connection, ignore the packet
			if(!c->linger)
			{
				return false;
			}

			// otherwise, remove local connection and it will be replaced with external

			replacing = true;
			lastReport = c->lastReport;

			d->removeConnection(c);
			delete c;
		}
	}

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

	QHash<QByteArray, Private::ConnectionInfo*> &extConnectionInfoById = d->externalConnectionInfoByFrom[packet.from];

	if(packet.type == StatsPacket::Connected)
	{
		// add/update
		Private::ConnectionInfo *c = extConnectionInfoById.value(packet.connectionId);
		if(!c)
		{
			c = new Private::ConnectionInfo;
			c->timerType = Private::TimerBase::Type::ExternalConnection;
			c->id = packet.connectionId;
			c->routeId = packet.route;
			c->type = packet.connectionType == StatsPacket::Http ? Http : WebSocket;
			c->peerAddress = packet.peerAddress;
			c->ssl = packet.ssl;
			c->lastReport = lastReport;
			c->from = packet.from;
			c->lastActive = now;
			d->insertExternalConnection(c);

			d->updateConnectionsMax(c->routeId, now);

			// only count a minute if we weren't replacing
			if(!replacing)
			{
				Private::Report *report = d->getOrCreateReport(c->routeId);

				// minutes are rounded up so count one immediately
				report->addConnectionsMinutes(1, now);
				d->combinedReport.addConnectionsMinutes(1, now);
			}
		}
		else
		{
			c->ttl = packet.ttl;

			c->lastActive = now;
			d->wheelAdd(c->lastActive + d->connectionTtl, c);
		}

		d->updateConnectionsMinutes(c, now);
	}
	else // Disconnected
	{
		Private::ConnectionInfo *c = extConnectionInfoById.value(packet.connectionId);
		if(c)
		{
			QByteArray routeId = c->routeId;

			d->updateConnectionsMinutes(c, now);
			d->removeExternalConnection(c);
			delete c;

			d->updateConnectionsMax(routeId, now);
		}
	}

	return replacing;
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
