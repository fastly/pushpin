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

	StatsManager(QObject *parent = 0);
	~StatsManager();

	void setInstanceId(const QByteArray &instanceId);
	void setIpcFileMode(int mode);
	bool setSpec(const QString &spec);
	void setReportsEnabled(bool on);

	// routeId may be empty for non-identified route

	void addActivity(const QByteArray &routeId, int count = 1);
	void addMessage(const QString &channel, const QString &itemId, const QString &transport, int count = 1);

	void addConnection(const QByteArray &id, const QByteArray &routeId, ConnectionType type, const QHostAddress &peerAddress, bool ssl, bool quiet);
	void removeConnection(const QByteArray &id, bool linger);

	void addSubscription(const QString &mode, const QString &channel);

	// NOTE: may emit unsubscribed immediately (not DOR-DS)
	void removeSubscription(const QString &mode, const QString &channel, bool linger);

	// for reporting only
	void addMessageReceived(const QByteArray &routeId);
	void addMessageSent(const QByteArray &routeId, const QString &transport);

	bool checkConnection(const QByteArray &id);

	// for reporting
	void processExternalPacket(const StatsPacket &packet);

	// directly send, for proxy->handler passthrough
	void sendPacket(const StatsPacket &packet);

signals:
	void connectionsRefreshed(const QList<QByteArray> &ids);
	void unsubscribed(const QString &mode, const QString &channel);
	void reported(const StatsPacket &packet);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
