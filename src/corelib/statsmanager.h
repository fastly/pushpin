/*
 * Copyright (C) 2014-2023 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
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

#ifndef STATSMANAGER_H
#define STATSMANAGER_H

#include <QObject>
#include "stats.h"

class QHostAddress;

class StatsPacket;

class StatsManager : public QObject
{
	Q_OBJECT

public:
	enum ConnectionType
	{
		Http,
		WebSocket
	};

	enum Format
	{
		TnetStringFormat,
		JsonFormat
	};

	StatsManager(int connectionsMax, int subscriptionsMax, QObject *parent = 0);
	~StatsManager();

	bool connectionSendEnabled() const;

	void setInstanceId(const QByteArray &instanceId);
	void setIpcFileMode(int mode);
	bool setSpec(const QString &spec);
	void setConnectionSendEnabled(bool enabled);
	void setConnectionsMaxSendEnabled(bool enabled);
	void setConnectionTtl(int secs);
	void setConnectionsMaxTtl(int secs);
	void setSubscriptionTtl(int secs);
	void setSubscriptionLinger(int secs);
	void setReportInterval(int secs);
	void setOutputFormat(Format format);
	bool setPrometheusPort(const QString &port);
	void setPrometheusPrefix(const QString &prefix);

	// routeId may be empty for non-identified route

	void addActivity(const QByteArray &routeId, quint32 count = 1);
	void addMessage(const QString &channel, const QString &itemId, const QString &transport, quint32 count = 1, int blocks = -1);

	void addConnection(const QByteArray &id, const QByteArray &routeId, ConnectionType type, const QHostAddress &peerAddress, bool ssl, bool quiet, int reportOffset = -1);
	int removeConnection(const QByteArray &id, bool linger); // return unreported time

	// manager automatically refreshes, but it may be useful to force a
	//   send before removing with linger
	void refreshConnection(const QByteArray &id);

	void addSubscription(const QString &mode, const QString &channel, quint32 subscriberCount);

	// NOTE: may emit unsubscribed immediately (not DOR-DS)
	void removeSubscription(const QString &mode, const QString &channel, bool linger);

	// for reporting and combined
	void addMessageReceived(const QByteArray &routeId, int blocks = -1);
	void addMessageSent(const QByteArray &routeId, const QString &transport, int blocks = -1);
	void incCounter(const QByteArray &routeId, Stats::Counter c, quint32 count = 1);

	// for combined only
	void addRequestsReceived(quint32 count);

	bool checkConnection(const QByteArray &id) const;

	// conn, conn-max, and report packets received from the proxy should be
	// passed into this method. returns true if the packet should not also be
	// forwarded on
	bool processExternalPacket(const StatsPacket &packet, bool mergeConnectionReport);

	// directly send, for proxy->handler passthrough
	void sendPacket(const StatsPacket &packet);

	void flushReport(const QByteArray &routeId);

	qint64 lastRetrySeq() const;

	StatsPacket getConnMaxPacket(const QByteArray &routeId);
	void setRetrySeq(const QByteArray &routeId, int value);

signals:
	void connectionsRefreshed(const QList<QByteArray> &ids);
	void unsubscribed(const QString &mode, const QString &channel);
	void reported(const QList<StatsPacket> &packet);
	void connMax(const StatsPacket &packet);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
