/*
 * Copyright (C) 2014-2023 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $FANOUT_END_LICENSE$
 */

#include "statsmanager.h"

#include <assert.h>
#include <QVector>
#include <QDateTime>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include "qzmqsocket.h"
#include "timerwheel.h"
#include "log.h"
#include "tnetstring.h"
#include "httpheaders.h"
#include "simplehttpserver.h"
#include "zutil.h"
#include "rtimer.h"

// make this somewhat big since PUB is lossy
#define OUT_HWM 200000

#define ACTIVITY_TIMEOUT 100
#define REFRESH_INTERVAL 1000
#define EXTERNAL_CONNECTIONS_MAX_INTERVAL 10000
#define EXPIRE_MAX 10000

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
		qint64 retrySeq;
		QByteArray from; // external or linger source
		int ttl; // external
		qint64 lastActive; // external

		ConnectionInfo() :
			ssl(false),
			lastRefresh(-1),
			refreshBucket(-1),
			linger(false),
			lastReport(-1),
			retrySeq(-1),
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
		quint32 subscriberCount;
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

	class ConnectionsMax
	{
	public:
		int retrySeq;
		quint32 currentValue;
		quint32 maxValue;
		quint32 lastSentValue;
		qint64 lastSentTime;

		ConnectionsMax() :
			retrySeq(-1),
			currentValue(0),
			maxValue(0),
			lastSentValue(0),
			lastSentTime(-1)
		{
		}

		quint32 valueToSend()
		{
			if(maxValue > lastSentValue)
				return maxValue;
			else
				return currentValue;
		}
	};

	class ExternalConnectionsMax
	{
	public:
		quint64 value;
		qint64 expires;

		ExternalConnectionsMax() :
			value(0),
			expires(0)
		{
		}
	};

	class ConnectionsMaxes
	{
	public:
		QHash<QByteArray, ConnectionsMax> maxes;
		QSet<QByteArray> needSend;
		qint64 lastRefresh;

		ConnectionsMaxes() :
			lastRefresh(0)
		{
		}
	};

	class ExternalConnectionsMaxes
	{
	public:
		QHash<QByteArray, ExternalConnectionsMax> maxes;

		quint32 total() const
		{
			quint32 count = 0;

			QHashIterator<QByteArray, ExternalConnectionsMax> it(maxes);
			while(it.hasNext())
			{
				it.next();
				const ExternalConnectionsMax &cm = it.value();

				count += cm.value;
			}

			return count;
		}
	};

	class RetryInfo
	{
	public:
		quint64 nextSeq;
		QMap<quint64, ConnectionInfo*> connectionInfoBySeq;

		RetryInfo() :
			nextSeq(0)
		{
		}
	};

	class Report
	{
	public:
		QByteArray routeId;
		quint32 connectionsMax;
		bool connectionsMaxStale;
		quint32 connectionsMinutes;
		quint32 messagesReceived;
		quint32 messagesSent;
		quint32 httpResponseMessagesSent;
		int blocksReceived;
		int blocksSent;
		quint32 requestsReceived;
		Stats::Counters counters;
		qint64 lastUpdate;
		qint64 startTime;
		QHash<QByteArray, Report> externalReports;

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
				requestsReceived == 0 &&
				counters.isEmpty());
		}

		quint32 externalConnectionsMinutes() const
		{
			quint32 count = 0;

			QHashIterator<QByteArray, Report> it(externalReports);
			while(it.hasNext())
			{
				it.next();
				const Report &r = it.value();

				count += r.connectionsMinutes;
			}

			return count;
		}

		void addConnectionsMinutes(quint32 mins, qint64 now)
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

		void addRequestsReceived(quint32 count, qint64 now)
		{
			requestsReceived += count;

			lastUpdate = now;
		}

		void incCounter(Stats::Counter c, quint32 count, qint64 now)
		{
			counters.inc(c, count);

			lastUpdate = now;
		}

		void addCounters(const Stats::Counters &other, qint64 now)
		{
			counters.add(other);

			lastUpdate = now;
		}
	};

	class Counts
	{
	public:
		quint32 requestsReceived;

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
	bool connectionSend;
	bool connectionsMaxSend;
	int connectionTtl;
	int connectionsMaxTtl;
	int connectionLinger;
	int subscriptionTtl;
	int subscriptionLinger;
	int reportInterval;
	QZmq::Socket *sock;
	SimpleHttpServer *prometheusServer;
	QString prometheusPrefix;
	QList<PrometheusMetric> prometheusMetrics;
	QHash<QByteArray, quint32> routeActivity;
	QHash<QByteArray, ConnectionInfo*> connectionInfoById;
	QHash<QByteArray, QSet<ConnectionInfo*> > connectionInfoByRoute;
	QHash<QByteArray, RetryInfo> retryInfoBySource;
	QVector<QSet<ConnectionInfo*> > connectionInfoRefreshBuckets;
	int currentConnectionInfoRefreshBucket;
	QHash<QByteArray, QHash<QByteArray, ConnectionInfo*> > externalConnectionInfoByFrom;
	QHash<QByteArray, QSet<ConnectionInfo*> > externalConnectionInfoByRoute;
	QHash<SubscriptionKey, Subscription*> subscriptionsByKey;
	QVector<QSet<Subscription*> > subscriptionRefreshBuckets;
	int currentSubscriptionRefreshBucket;
	ConnectionsMaxes connectionsMaxes;
	QHash<QByteArray, ExternalConnectionsMaxes> externalConnectionsMaxes;
	TimerWheel wheel;
	qint64 startTime;
	QHash<QByteArray, Report*> reports;
	Counts combinedCounts;
	Report combinedReport;
	std::unique_ptr<RTimer> activityTimer;
	std::unique_ptr<RTimer> reportTimer;
	std::unique_ptr<RTimer> refreshTimer;
	std::unique_ptr<RTimer> externalConnectionsMaxTimer;
	Connection activityTimerConnection;
	Connection reportTimerConnection;
	Connection refreshTimerConnection;
	Connection externalConnectionsMaxTimerConnection;
	Connection promServerConnection;

	Private(StatsManager *_q, int _connectionsMax, int _subscriptionsMax) :
		QObject(_q),
		q(_q),
		connectionsMax(_connectionsMax),
		subscriptionsMax(_subscriptionsMax),
		ipcFileMode(-1),
		outputFormat(TnetStringFormat),
		connectionSend(false),
		connectionsMaxSend(false),
		connectionTtl(120 * 1000),
		connectionsMaxTtl(60 * 1000),
		connectionLinger(60 * 1000),
		subscriptionTtl(60 * 1000),
		subscriptionLinger(60 * 1000),
		reportInterval(10 * 1000),
		sock(0),
		prometheusServer(0),
		currentConnectionInfoRefreshBucket(0),
		currentSubscriptionRefreshBucket(0),
		wheel(TimerWheel((_connectionsMax * 2) + _subscriptionsMax))
	{
		activityTimer = std::make_unique<RTimer>();
		activityTimerConnection = activityTimer->timeout.connect(boost::bind(&Private::activity_timeout, this));
		activityTimer->setSingleShot(true);

		refreshTimer = std::make_unique<RTimer>();
		refreshTimerConnection = refreshTimer->timeout.connect(boost::bind(&Private::refresh_timeout, this));
		refreshTimer->start(REFRESH_INTERVAL);

		externalConnectionsMaxTimer = std::make_unique<RTimer>();
		externalConnectionsMaxTimerConnection = externalConnectionsMaxTimer->timeout.connect(boost::bind(&Private::externalConnectionsMax_timeout, this));
		externalConnectionsMaxTimer->start(EXTERNAL_CONNECTIONS_MAX_INTERVAL);

		setupConnectionBuckets();
		setupSubscriptionBuckets();

		prometheusMetrics += PrometheusMetric(PrometheusMetric::RequestReceived, "request_received", "counter", "Number of requests received");
		prometheusMetrics += PrometheusMetric(PrometheusMetric::ConnectionConnected, "connection_connected", "gauge", "Number of concurrent connections");
		prometheusMetrics += PrometheusMetric(PrometheusMetric::ConnectionMinute, "connection_minute", "counter", "Number of minutes clients have been connected");
		prometheusMetrics += PrometheusMetric(PrometheusMetric::MessageReceived, "message_received", "counter", "Number of messages received by the publish API");
		prometheusMetrics += PrometheusMetric(PrometheusMetric::MessageSent,"message_sent", "counter", "Number of messages sent to clients");

		startTime = QDateTime::currentMSecsSinceEpoch();

		connectionsMaxes.lastRefresh = startTime;
	}

	~Private()
	{
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
		promServerConnection = prometheusServer->requestReady.connect(boost::bind(&Private::prometheus_requestReady, this));

		if(portStr.startsWith("ipc://"))
		{
			if(!prometheusServer->listenLocal(portStr.mid(6)))
			{
				promServerConnection.disconnect();
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
				promServerConnection.disconnect();
				delete prometheusServer;

				return false;
			}
		}

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
			reportTimer = std::make_unique<RTimer>();
			reportTimerConnection = reportTimer->timeout.connect(boost::bind(&Private::report_timeout, this));
			reportTimer->start(reportInterval);
		}
		else if(reportInterval <= 0 && reportTimer)
		{
			reportTimerConnection.disconnect();
			reportTimer.reset();
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

		if(c->retrySeq >= 0)
		{
			RetryInfo &ri = retryInfoBySource[c->from];
			ri.connectionInfoBySeq.remove(c->retrySeq);

			// FIXME: we keep the source entry even when there are no more
			// connections, to avoid resetting the seq value. if there is
			// a lot of proxy instance churn, retryInfoBySource could
			// fill up with unused entries that will never be cleaned up.
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

	void removeLingeringConnections(const QByteArray &source, quint64 retrySeq)
	{
		if(!retryInfoBySource.contains(source))
			return;

		RetryInfo &ri = retryInfoBySource[source];

		// invalid retry seq
		if(retrySeq >= ri.nextSeq)
			return;

		QList<ConnectionInfo*> toRemove;

		QMap<quint64, ConnectionInfo*>::iterator it = ri.connectionInfoBySeq.find(retrySeq);
		while(it != ri.connectionInfoBySeq.end())
		{
			toRemove += it.value();

			if(it == ri.connectionInfoBySeq.begin())
				break;

			--it;
		}

		foreach(ConnectionInfo *c, toRemove)
		{
			removeConnection(c);
			delete c;
		}
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

	ConnectionsMax & getOrCreateConnectionsMax(const QByteArray &routeId)
	{
		if(!connectionsMaxes.maxes.contains(routeId))
			connectionsMaxes.maxes.insert(routeId, ConnectionsMax());

		return connectionsMaxes.maxes[routeId];
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
		else if(packet.type == StatsPacket::Counts)
			prefix = "counts";
		else // ConnectionsMax
			prefix = "conn-max";

		QVariant vpacket = packet.toVariant();

		QByteArray buf;
		if(outputFormat == TnetStringFormat)
		{
			buf = prefix + " T" + TnetString::fromVariant(vpacket);
		}
		else if(outputFormat == JsonFormat)
		{
			QJsonObject obj = QJsonObject::fromVariantHash(vpacket.toHash());
			buf = prefix + " J" + QJsonDocument(obj).toJson(QJsonDocument::Compact);
		}

		if(!buf.isEmpty())
		{
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				log_debug("stats: OUT %s %s", prefix.data(), qPrintable(TnetString::variantToString(vpacket, -1)));

			sock->write(QList<QByteArray>() << buf);
		}
	}

	void sendActivity(const QByteArray &routeId, quint32 count)
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

	void sendMessage(const QString &channel, const QString &itemId, const QString &transport, quint32 count, int blocks)
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
		if(!sock || !connectionSend)
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
		if(!sock || !connectionSend)
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

	void sendConnectionsMax(const QByteArray &routeId, ConnectionsMax *cm, qint64 now)
	{
		q->connMax(getConnMaxPacket(routeId, cm, now));
	}

	void updateConnectionsMax(const QByteArray &routeId, qint64 now)
	{
		quint32 localConns = connectionInfoByRoute.value(routeId).count();
		quint32 extConns = externalConnectionInfoByRoute.value(routeId).count();

		quint32 extConnsMax = 0;
		if(externalConnectionsMaxes.contains(routeId))
			extConnsMax = externalConnectionsMaxes[routeId].total();

		quint32 conns = localConns + extConns + extConnsMax;

		if(connectionsMaxSend)
		{
			ConnectionsMax &cm = getOrCreateConnectionsMax(routeId);

			cm.currentValue = conns;
			cm.maxValue = qMax(cm.maxValue, conns);

			if(cm.valueToSend() != cm.lastSentValue)
			{
				connectionsMaxes.needSend.insert(routeId);

				if(!activityTimer->isActive())
					activityTimer->start(ACTIVITY_TIMEOUT);
			}
			else
			{
				connectionsMaxes.needSend.remove(routeId);
			}
		}

		if(reportInterval > 0)
		{
			Report *report = getOrCreateReport(routeId);

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

		for(int i = 0; i < EXPIRE_MAX; ++i)
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

						q->unsubscribed(mode, channel);
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
			q->connectionsRefreshed(refreshedConnIds);

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
			q->connectionsRefreshed(refreshedIds);

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

	void refreshConnectionsMaxes(qint64 now)
	{
		if(now < connectionsMaxes.lastRefresh + (connectionsMaxTtl * 3 / 4))
			return;

		connectionsMaxes.lastRefresh = now;

		QList<QByteArray> toRemove;

		QMutableHashIterator<QByteArray, ConnectionsMax> it(connectionsMaxes.maxes);
		while(it.hasNext())
		{
			it.next();
			const QByteArray &routeId = it.key();
			ConnectionsMax &cm = it.value();

			if(cm.valueToSend() == 0)
			{
				if(cm.lastSentTime >= 0 && now >= cm.lastSentTime + connectionsMaxTtl)
					toRemove += routeId;

				continue;
			}

			sendConnectionsMax(routeId, &cm, now);
		}

		foreach(const QByteArray &routeId, toRemove)
			connectionsMaxes.maxes.remove(routeId);
	}

	void expireExternalConnectionsMaxes(qint64 now)
	{
		QMutableHashIterator<QByteArray, ExternalConnectionsMaxes> it(externalConnectionsMaxes);
		while(it.hasNext())
		{
			it.next();
			QHash<QByteArray, ExternalConnectionsMax> &maxesForRoute = it.value().maxes;

			QMutableHashIterator<QByteArray, ExternalConnectionsMax> rit(maxesForRoute);
			while(rit.hasNext())
			{
				rit.next();
				ExternalConnectionsMax &cm = rit.value();

				if(now >= cm.expires)
					rit.remove();
			}

			if(maxesForRoute.isEmpty())
				it.remove();
		}
	}

	void mergeExternalConnectionsMax(const StatsPacket &packet, qint64 now)
	{
		if(packet.retrySeq >= 0)
			removeLingeringConnections(packet.from, (quint64)packet.retrySeq);

		QHash<QByteArray, ExternalConnectionsMax> &maxes = externalConnectionsMaxes[packet.route].maxes;

		if(!maxes.contains(packet.from))
			maxes.insert(packet.from, ExternalConnectionsMax());

		ExternalConnectionsMax &cm = maxes[packet.from];

		cm.value = (quint32)qMax(packet.connectionsMax, 0);
		cm.expires = now + (qMax(packet.ttl, 0) * 1000);

		updateConnectionsMax(packet.route, now);
	}

	void mergeExternalReport(const StatsPacket &packet, bool includeConnections)
	{
		if(reportInterval <= 0)
			return;

		Report *report = getOrCreateReport(packet.route);

		Stats::Counters counters;

		counters.inc(Stats::ClientHeaderBytesReceived, qMax(packet.clientHeaderBytesReceived, 0));
		counters.inc(Stats::ClientHeaderBytesSent, qMax(packet.clientHeaderBytesSent, 0));
		counters.inc(Stats::ClientContentBytesReceived, qMax(packet.clientContentBytesReceived, 0));
		counters.inc(Stats::ClientContentBytesSent, qMax(packet.clientContentBytesSent, 0));
		counters.inc(Stats::ClientMessagesReceived, qMax(packet.clientMessagesReceived, 0));
		counters.inc(Stats::ClientMessagesSent, qMax(packet.clientMessagesSent, 0));
		counters.inc(Stats::ServerHeaderBytesReceived, qMax(packet.serverHeaderBytesReceived, 0));
		counters.inc(Stats::ServerHeaderBytesSent, qMax(packet.serverHeaderBytesSent, 0));
		counters.inc(Stats::ServerContentBytesReceived, qMax(packet.serverContentBytesReceived, 0));
		counters.inc(Stats::ServerContentBytesSent, qMax(packet.serverContentBytesSent, 0));
		counters.inc(Stats::ServerMessagesReceived, qMax(packet.serverMessagesReceived, 0));
		counters.inc(Stats::ServerMessagesSent, qMax(packet.serverMessagesSent, 0));

		qint64 now = QDateTime::currentMSecsSinceEpoch();

		report->addCounters(counters, now);
		combinedReport.addCounters(counters, now);

		if(includeConnections)
		{
			if(!report->externalReports.contains(packet.from))
				report->externalReports[packet.from] = Report();

			Report &r = report->externalReports[packet.from];

			r.connectionsMinutes += qMax(packet.connectionsMinutes, 0);
		}
	}

	StatsPacket reportToPacket(Report *report, const QByteArray &routeId, qint64 now)
	{
		if(report->connectionsMaxStale)
			updateConnectionsMax(routeId, now);

		StatsPacket p;
		p.type = StatsPacket::Report;
		p.from = instanceId;
		p.route = routeId;

		p.connectionsMax = report->connectionsMax;
		p.connectionsMinutes = report->connectionsMinutes + report->externalConnectionsMinutes();
		p.messagesReceived = report->messagesReceived;
		p.messagesSent = report->messagesSent;
		p.httpResponseMessagesSent = report->httpResponseMessagesSent;
		p.blocksReceived = report->blocksReceived;
		p.blocksSent = report->blocksSent;
		p.duration = now - report->startTime;

		p.clientHeaderBytesReceived = report->counters.get(Stats::ClientHeaderBytesReceived);
		p.clientHeaderBytesSent = report->counters.get(Stats::ClientHeaderBytesSent);
		p.clientContentBytesReceived = report->counters.get(Stats::ClientContentBytesReceived);
		p.clientContentBytesSent = report->counters.get(Stats::ClientContentBytesSent);
		p.clientMessagesReceived = report->counters.get(Stats::ClientMessagesReceived);
		p.clientMessagesSent = report->counters.get(Stats::ClientMessagesSent);
		p.serverHeaderBytesReceived = report->counters.get(Stats::ServerHeaderBytesReceived);
		p.serverHeaderBytesSent = report->counters.get(Stats::ServerHeaderBytesSent);
		p.serverContentBytesReceived = report->counters.get(Stats::ServerContentBytesReceived);
		p.serverContentBytesSent = report->counters.get(Stats::ServerContentBytesSent);
		p.serverMessagesReceived = report->counters.get(Stats::ServerMessagesReceived);
		p.serverMessagesSent = report->counters.get(Stats::ServerMessagesSent);

		report->startTime = now;
		report->connectionsMaxStale = true;
		report->connectionsMinutes = 0;
		report->messagesReceived = 0;
		report->messagesSent = 0;
		report->httpResponseMessagesSent = 0;
		report->blocksReceived = -1;
		report->blocksSent = -1;
		report->counters.reset();
		report->externalReports.clear();

		return p;
	}

	void flushReport(const QByteArray &routeId)
	{
		Report *report = getOrCreateReport(routeId);

		qint64 now = QDateTime::currentMSecsSinceEpoch();

		StatsPacket p = reportToPacket(report, routeId, now);

		if(report->isEmpty())
		{
			removeReport(report);
			delete report;
		}

		if(sock)
			write(p);

		q->reported(QList<StatsPacket>() << p);
	}

	StatsPacket getConnMaxPacket(const QByteArray &routeId, ConnectionsMax *cm, qint64 now)
	{
		quint32 value = cm->valueToSend();

		StatsPacket p;
		p.type = StatsPacket::ConnectionsMax;
		p.from = instanceId;
		p.route = routeId;
		p.connectionsMax = value;
		p.ttl = connectionsMaxTtl / 1000;
		p.retrySeq = cm->retrySeq;

		cm->lastSentValue = value;
		cm->lastSentTime = now;
		cm->maxValue = cm->currentValue;

		return p;
	}

private:
	void activity_timeout()
	{
		QHashIterator<QByteArray, quint32> it(routeActivity);
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

		qint64 now = QDateTime::currentMSecsSinceEpoch();

		QSet<QByteArray> needSendNext;

		foreach(const QByteArray &routeId, connectionsMaxes.needSend)
		{
			ConnectionsMax &cm = connectionsMaxes.maxes[routeId];

			if(cm.valueToSend() == cm.lastSentValue)
				continue;

			sendConnectionsMax(routeId, &cm, now);

			if(cm.valueToSend() != cm.lastSentValue)
				needSendNext.insert(routeId);
		}

		connectionsMaxes.needSend = needSendNext;

		if(!connectionsMaxes.needSend.isEmpty() && !activityTimer->isActive())
			activityTimer->start(ACTIVITY_TIMEOUT);
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

			StatsPacket p = reportToPacket(report, routeId, now);

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
			q->reported(reportPackets);
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
		refreshConnectionsMaxes(currentTime);
	}

	void externalConnectionsMax_timeout()
	{
		qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

		expireExternalConnectionsMaxes(currentTime);
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

		req->finished.connect(boost::bind(&SimpleHttpRequest::deleteLater, req));

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

bool StatsManager::connectionSendEnabled() const
{
	return d->connectionSend;
}

void StatsManager::setConnectionSendEnabled(bool enabled)
{
	d->connectionSend = enabled;
}

void StatsManager::setConnectionsMaxSendEnabled(bool enabled)
{
	d->connectionsMaxSend = enabled;
}

void StatsManager::setConnectionTtl(int secs)
{
	d->connectionTtl = secs * 1000;
	d->setupConnectionBuckets();
}

void StatsManager::setConnectionsMaxTtl(int secs)
{
	d->connectionsMaxTtl = secs * 1000;
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

void StatsManager::addActivity(const QByteArray &routeId, quint32 count)
{
	if(d->routeActivity.contains(routeId))
		d->routeActivity[routeId] += count;
	else
		d->routeActivity[routeId] = count;

	if(!d->activityTimer->isActive())
		d->activityTimer->start(ACTIVITY_TIMEOUT);
}

void StatsManager::addMessage(const QString &channel, const QString &itemId, const QString &transport, quint32 count, int blocks)
{
	d->sendMessage(channel, itemId, transport, count, blocks);
}

void StatsManager::addConnection(const QByteArray &id, const QByteArray &routeId, ConnectionType type, const QHostAddress &peerAddress, bool ssl, bool quiet, int reportOffset)
{
	qint64 now = QDateTime::currentMSecsSinceEpoch();

	bool replacing = false;
	qint64 lastReport = now;

	if(reportOffset >= 0)
		lastReport -= reportOffset;

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

	d->updateConnectionsMax(c->routeId, now);

	if(d->reportInterval > 0)
	{
		// only immediately count a minute if an offset wasn't set and we weren't replacing
		if(reportOffset < 0 && !replacing)
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

int StatsManager::removeConnection(const QByteArray &id, bool linger, const QByteArray &source)
{
	Private::ConnectionInfo *c = d->connectionInfoById.value(id);
	if(!c)
		return 0;

	qint64 now = QDateTime::currentMSecsSinceEpoch();
	QByteArray routeId = c->routeId;
	int unreportedTime = 0;

	if(d->reportInterval > 0)
		d->updateConnectionsMinutes(c, now);

	if(linger)
	{
		if(!c->linger)
		{
			c->linger = true;

			if(!d->retryInfoBySource.contains(source))
				d->retryInfoBySource.insert(source, Private::RetryInfo());

			Private::RetryInfo &ri = d->retryInfoBySource[source];

			c->from = source;
			c->retrySeq = (qint64)ri.nextSeq++;

			ri.connectionInfoBySeq.insert((quint64)c->retrySeq, c);

			// hack to ensure full linger time honored by refresh processing
			qint64 lingerStartTime = now + (d->connectionLinger - SHOULD_PROCESS_TIME(d->connectionTtl));

			c->lastRefresh = lingerStartTime;
			d->wheelAdd(c->lastRefresh + SHOULD_PROCESS_TIME(d->connectionTtl), c);
		}
	}
	else
	{
		if(now >= c->lastReport)
			unreportedTime = now - c->lastReport;

		d->sendDisconnected(c);
		d->removeConnection(c);
		delete c;
	}

	d->updateConnectionsMax(routeId, now);

	return unreportedTime;
}

void StatsManager::refreshConnection(const QByteArray &id)
{
	Private::ConnectionInfo *c = d->connectionInfoById.value(id);
	if(!c)
		return;

	d->sendConnected(c);
}

void StatsManager::addSubscription(const QString &mode, const QString &channel, quint32 subscriberCount)
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
		quint32 oldSubscriberCount = s->subscriberCount;

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

		unsubscribed(mode, channel);
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

void StatsManager::incCounter(const QByteArray &routeId, Stats::Counter c, quint32 count)
{
	if(d->reportInterval <= 0)
		return;

	Private::Report *report = d->getOrCreateReport(routeId);

	qint64 now = QDateTime::currentMSecsSinceEpoch();

	report->incCounter(c, count, now);
	d->combinedReport.incCounter(c, count, now);
}

void StatsManager::addRequestsReceived(quint32 count)
{
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

bool StatsManager::processExternalPacket(const StatsPacket &packet, bool mergeConnectionReport)
{
	if(d->reportInterval <= 0)
		return false;

	if(packet.type == StatsPacket::Connected || packet.type == StatsPacket::Disconnected)
	{
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
	else if(packet.type == StatsPacket::ConnectionsMax)
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		d->mergeExternalConnectionsMax(packet, now);

		return true;
	}
	else if(packet.type == StatsPacket::Report)
	{
		d->mergeExternalReport(packet, mergeConnectionReport);

		return true;
	}

	return false;
}

void StatsManager::sendPacket(const StatsPacket &packet)
{
	if(!d->sock)
		return;

	StatsPacket p = packet;
	p.from = d->instanceId;
	d->write(p);
}

void StatsManager::flushReport(const QByteArray &routeId)
{
	d->flushReport(routeId);
}

qint64 StatsManager::lastRetrySeq(const QByteArray &source) const
{
	if(!d->retryInfoBySource.contains(source))
		return -1;

	Private::RetryInfo &ri = d->retryInfoBySource[source];

	return ((qint64)ri.nextSeq) - 1;
}

StatsPacket StatsManager::getConnMaxPacket(const QByteArray &routeId)
{
	Private::ConnectionsMax &cm = d->getOrCreateConnectionsMax(routeId);

	qint64 now = QDateTime::currentMSecsSinceEpoch();

	return d->getConnMaxPacket(routeId, &cm, now);
}

void StatsManager::setRetrySeq(const QByteArray &routeId, int value)
{
	Private::ConnectionsMax &cm = d->getOrCreateConnectionsMax(routeId);

	cm.retrySeq = value;
}

#include "statsmanager.moc"
