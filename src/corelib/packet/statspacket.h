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

#ifndef STATSPACKET_H
#define STATSPACKET_H

#include <QByteArray>
#include <QVariant>
#include <QHostAddress>

class StatsPacket
{
public:
	enum Type
	{
		Activity,
		Message,
		Connected,
		Disconnected,
		Subscribed,
		Unsubscribed,
		Report
	};

	enum ConnectionType
	{
		Http,
		WebSocket
	};

	Type type;
	QByteArray from;
	QByteArray route;
	int count; // activity
	QByteArray connectionId; // connected, disconnected
	ConnectionType connectionType; // connected
	QHostAddress peerAddress; // connected
	bool ssl; // connected
	int ttl; // connected, subscribed
	QByteArray mode; // subscribed, unsubscribed
	QByteArray channel; // message, subscribed, unsubscribed
	QByteArray itemId; // message
	QByteArray transport; // message
	int connectionsMax; // report
	int connectionsMinutes; // report
	int messagesReceived; // report
	int messagesSent; // report
	int httpResponseMessagesSent; // report

	StatsPacket() :
		type((Type)-1),
		count(-1),
		connectionType((ConnectionType)-1),
		ssl(false),
		ttl(-1),
		connectionsMax(-1),
		connectionsMinutes(-1),
		messagesReceived(-1),
		messagesSent(-1),
		httpResponseMessagesSent(-1)
	{
	}

	QVariant toVariant() const;
	bool fromVariant(const QByteArray &type, const QVariant &in);
};

#endif
