/*
 * Copyright (C) 2014-2022 Fanout, Inc.
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
		Counts,
		ConnectionsMax,
	};

	enum ConnectionType
	{
		Http,
		WebSocket
	};

	Type type;
	QByteArray from;
	QByteArray route;
	qint64 retrySeq; // connections max
	int count; // activity, message
	QByteArray connectionId; // connected, disconnected
	ConnectionType connectionType; // connected
	QHostAddress peerAddress; // connected
	bool ssl; // connected
	int ttl; // connected, subscribed, connections max
	QByteArray mode; // subscribed, unsubscribed
	QByteArray channel; // message, subscribed, unsubscribed
	QByteArray itemId; // message
	QByteArray transport; // message
	int blocks; // message
	int subscribers; // subscribed
	int connectionsMax; // report, connections max
	int connectionsMinutes; // report
	int messagesReceived; // report
	int messagesSent; // report
	int httpResponseMessagesSent; // report
	int blocksReceived; // report
	int blocksSent; // report
	int duration; // report
	int requestsReceived; // counts
	int clientHeaderBytesReceived; // report
	int clientHeaderBytesSent; // report
	int clientContentBytesReceived; // report
	int clientContentBytesSent; // report
	int clientMessagesReceived; // report
	int clientMessagesSent; // report
	int serverHeaderBytesReceived; // report
	int serverHeaderBytesSent; // report
	int serverContentBytesReceived; // report
	int serverContentBytesSent; // report
	int serverMessagesReceived; // report
	int serverMessagesSent; // report

	StatsPacket() :
		type((Type)-1),
		retrySeq(-1),
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
		requestsReceived(-1),
		clientHeaderBytesReceived(-1),
		clientHeaderBytesSent(-1),
		clientContentBytesReceived(-1),
		clientContentBytesSent(-1),
		clientMessagesReceived(-1),
		clientMessagesSent(-1),
		serverHeaderBytesReceived(-1),
		serverHeaderBytesSent(-1),
		serverContentBytesReceived(-1),
		serverContentBytesSent(-1),
		serverMessagesReceived(-1),
		serverMessagesSent(-1)
	{
	}

	QVariant toVariant() const;
	bool fromVariant(const QByteArray &type, const QVariant &in);
};

#endif
