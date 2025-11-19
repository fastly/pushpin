/*
 * Copyright (C) 2014-2023 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
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

#include "packet/statspacket.h"
#include "stats.h"
#include <boost/signals2.hpp>

class QHostAddress;

/// Collects and reports statistics via ZeroMQ (internal) and Prometheus (external) on:
/// - Connections
/// - Subscriptions
/// - Messages sent/received
class StatsManager
{
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

	StatsManager(int connectionsMax, int subscriptionsMax, int prometheusConnectionsMax);
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

	// RouteId may be empty for non-identified route

	void addActivity(const QByteArray &routeId, uint32_t count = 1);
	void addMessage(const QString &channel, const QString &itemId, const QString &transport, uint32_t count = 1, int blocks = -1);

	void addConnection(const QByteArray &id, const QByteArray &routeId, ConnectionType type, const QHostAddress &peerAddress, bool ssl, bool quiet, int reportOffset = -1);
	int removeConnection(const QByteArray &id, bool linger, const QByteArray &source = QByteArray()); // Return unreported time

	// Manager automatically refreshes, but it may be useful to force a
	// send before removing with linger
	void refreshConnection(const QByteArray &id);

	void addSubscription(const QString &mode, const QString &channel, uint32_t subscriberCount);

	// NOTE: may emit unsubscribed immediately (not DOR-DS)
	void removeSubscription(const QString &mode, const QString &channel, bool linger);

	// For reporting and combined
	void addMessageReceived(const QByteArray &routeId, int blocks = -1);
	void addMessageSent(const QByteArray &routeId, const QString &transport, int blocks = -1);
	void incCounter(const QByteArray &routeId, Stats::Counter c, uint32_t count = 1);

	// For combined only
	void addRequestsReceived(uint32_t count);

	bool checkConnection(const QByteArray &id) const;

	// Conn, conn-max, and report packets received from the proxy should be
	// passed into this method. returns true if the packet should not also be
	// forwarded on
	bool processExternalPacket(const StatsPacket &packet, bool mergeConnectionReport);

	// Directly send, for proxy->handler pass-through
	void sendPacket(const StatsPacket &packet);

	void flushReport(const QByteArray &routeId);

	int64_t lastRetrySeq(const QByteArray &source) const;

	StatsPacket getConnMaxPacket(const QByteArray &routeId);
	void setRetrySeq(const QByteArray &routeId, int value);

	boost::signals2::signal<void(const QList<QByteArray>&)> connectionsRefreshed;
	boost::signals2::signal<void(const QString&, const QString&)> unsubscribed;
	boost::signals2::signal<void(const QList<StatsPacket>&)> reported;
	boost::signals2::signal<void(const StatsPacket&)> connMax;

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
