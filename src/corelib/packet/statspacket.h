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
		Report,
		Counts
	};

	enum ConnectionType
	{
		Http,
		WebSocket
	};

	Type type;
	QByteArray from;
	QByteArray route;
	int count; // activity, message
	QByteArray connectionId; // connected, disconnected
	ConnectionType connectionType; // connected
	QHostAddress peerAddress; // connected
	bool ssl; // connected
	int ttl; // connected, subscribed
	QByteArray mode; // subscribed, unsubscribed
	QByteArray channel; // message, subscribed, unsubscribed
	QByteArray itemId; // message
	QByteArray transport; // message
	int blocks; // message
	int subscribers; // subscribed
	int connectionsMax; // report
	int connectionsMinutes; // report
	int messagesReceived; // report
	int messagesSent; // report
	int httpResponseMessagesSent; // report
	int blocksReceived; // report
	int blocksSent; // report
	int duration; // report
	int requestsReceived; // counts

	StatsPacket() :
		type((Type)-1),
		count(-1),
		connectionType((ConnectionType)-1),
		ssl(false),
		ttl(-1),
		blocks(-1),
		subscribers(-1),
		connectionsMax(-1),
		connectionsMinutes(-1),
		messagesReceived(-1),
		messagesSent(-1),
		httpResponseMessagesSent(-1),
		blocksReceived(-1),
		blocksSent(-1),
		duration(-1),
		requestsReceived(-1)
	{
	}

	QVariant toVariant() const;
	bool fromVariant(const QByteArray &type, const QVariant &in);
};

#endif
