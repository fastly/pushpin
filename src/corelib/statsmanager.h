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

#ifndef STATSMANAGER_H
#define STATSMANAGER_H

#include <QObject>

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

	void setInstanceId(const QByteArray &instanceId);
	void setIpcFileMode(int mode);
	bool setSpec(const QString &spec);
	void setConnectionTtl(int secs);
	void setSubscriptionTtl(int secs);
	void setSubscriptionLinger(int secs);
	void setReportInterval(int secs);
	void setOutputFormat(Format format);
	bool setPrometheusPort(const QString &port);
	void setPrometheusPrefix(const QString &prefix);

	// routeId may be empty for non-identified route

	void addActivity(const QByteArray &routeId, int count = 1);
	void addMessage(const QString &channel, const QString &itemId, const QString &transport, int count = 1, int blocks = -1);

	void addConnection(const QByteArray &id, const QByteArray &routeId, ConnectionType type, const QHostAddress &peerAddress, bool ssl, bool quiet);
	void removeConnection(const QByteArray &id, bool linger);

	// manager automatically refreshes, but it may be useful to force a
	//   send before removing with linger
	void refreshConnection(const QByteArray &id);

	void addSubscription(const QString &mode, const QString &channel, int subscriberCount);

	// NOTE: may emit unsubscribed immediately (not DOR-DS)
	void removeSubscription(const QString &mode, const QString &channel, bool linger);

	// for reporting only
	void addMessageReceived(const QByteArray &routeId, int blocks = -1);
	void addMessageSent(const QByteArray &routeId, const QString &transport, int blocks = -1);

	// for combined only
	void addRequestsReceived(int count);

	bool checkConnection(const QByteArray &id) const;

	// for reporting. return true if local connection was replaced
	bool processExternalPacket(const StatsPacket &packet);

	// directly send, for proxy->handler passthrough
	void sendPacket(const StatsPacket &packet);

signals:
	void connectionsRefreshed(const QList<QByteArray> &ids);
	void unsubscribed(const QString &mode, const QString &channel);
	void reported(const QList<StatsPacket> &packet);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
