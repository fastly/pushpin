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

#include "statspacket.h"

#include "qtcompat.h"

static bool tryGetInt(const QVariantHash &obj, const QString &name, int *result)
{
	if(obj.contains(name))
	{
		if(!canConvert(obj[name], QMetaType::Int))
			return false;

		*result = obj[name].toInt();
	}

	return true;
}

QVariant StatsPacket::toVariant() const
{
	QVariantHash obj;

	if(!from.isEmpty())
		obj["from"] = from;

	if(!route.isEmpty())
		obj["route"] = route;

	if(type == Activity)
	{
		int x = count;
		if(x < 0)
			x = 0;
		obj["count"] = x;
	}
	else if(type == Message)
	{
		obj["channel"] = channel;

		if(!itemId.isNull())
			obj["item-id"] = itemId;

		int x = count;
		if(x < 0)
			x = 0;
		obj["count"] = x;

		if(blocks >= 0)
			obj["blocks"] = blocks;

		obj["transport"] = transport;
	}
	else if(type == Connected || type == Disconnected)
	{
		obj["id"] = connectionId;

		if(type == Connected)
		{
			if(connectionType == WebSocket)
				obj["type"] = QByteArray("ws");
			else // Http
				obj["type"] = QByteArray("http");

			if(!peerAddress.isNull())
				obj["peer-address"] = peerAddress.toString().toUtf8();

			if(ssl)
				obj["ssl"] = true;

			obj["ttl"] = ttl;
		}
		else // Disconnected
		{
			obj["unavailable"] = true;
		}
	}
	else if(type == Subscribed || type == Unsubscribed)
	{
		obj["mode"] = mode;
		obj["channel"] = channel;

		if(type == Subscribed)
		{
			obj["ttl"] = ttl;

			if(subscribers >= 0)
				obj["subscribers"] = subscribers;
		}
		else // Unsubscribed
		{
			obj["unavailable"] = true;
		}
	}
	else if(type == Report)
	{
		if(connectionsMax != -1)
			obj["connections"] = connectionsMax;
		if(connectionsMinutes != -1)
			obj["minutes"] = connectionsMinutes;
		if(messagesReceived != -1)
			obj["received"] = messagesReceived;
		if(messagesSent != -1)
			obj["sent"] = messagesSent;
		if(httpResponseMessagesSent != -1)
			obj["http-response-sent"] = httpResponseMessagesSent;
		if(blocksReceived >= 0)
			obj["blocks-received"] = blocksReceived;
		if(blocksSent >= 0)
			obj["blocks-sent"] = blocksSent;
		if(duration >= 0)
			obj["duration"] = duration;

		if(clientHeaderBytesReceived >= 0)
			obj["client-header-bytes-received"] = clientHeaderBytesReceived;
		if(clientHeaderBytesSent >= 0)
			obj["client-header-bytes-sent"] = clientHeaderBytesSent;
		if(clientContentBytesReceived >= 0)
			obj["client-content-bytes-received"] = clientContentBytesReceived;
		if(clientContentBytesSent >= 0)
			obj["client-content-bytes-sent"] = clientContentBytesSent;
		if(clientMessagesReceived >= 0)
			obj["client-messages-received"] = clientMessagesReceived;
		if(clientMessagesSent >= 0)
			obj["client-messages-sent"] = clientMessagesSent;
		if(serverHeaderBytesReceived >= 0)
			obj["server-header-bytes-received"] = serverHeaderBytesReceived;
		if(serverHeaderBytesSent >= 0)
			obj["server-header-bytes-sent"] = serverHeaderBytesSent;
		if(serverContentBytesReceived >= 0)
			obj["server-content-bytes-received"] = serverContentBytesReceived;
		if(serverContentBytesSent >= 0)
			obj["server-content-bytes-sent"] = serverContentBytesSent;
		if(serverMessagesReceived >= 0)
			obj["server-messages-received"] = serverMessagesReceived;
		if(serverMessagesSent >= 0)
			obj["server-messages-sent"] = serverMessagesSent;
	}
	else if(type == Counts)
	{
		if(requestsReceived > 0)
			obj["requests-received"] = requestsReceived;
	}
	else // ConnectionsMax
	{
		obj["max"] = qMax(connectionsMax, 0);
		obj["ttl"] = qMax(ttl, 0);

		if(retrySeq >= 0)
			obj["retry-seq"] = (qint64)retrySeq;
	}

	return obj;
}

bool StatsPacket::fromVariant(const QByteArray &_type, const QVariant &in)
{
	if(typeId(in) != QMetaType::QVariantHash)
		return false;

	QVariantHash obj = in.toHash();

	if(obj.contains("from"))
	{
		if(typeId(obj["from"]) != QMetaType::QByteArray)
			return false;

		from = obj["from"].toByteArray();
	}

	if(obj.contains("route"))
	{
		if(typeId(obj["route"]) != QMetaType::QByteArray)
			return false;

		route = obj["route"].toByteArray();
	}

	if(_type == "activity")
	{
		type = Activity;

		if(!obj.contains("count") || !canConvert(obj["count"], QMetaType::Int))
			return false;

		count = obj["count"].toInt();
		if(count < 0)
			return false;
	}
	else if(_type == "message")
	{
		type = Message;

		if(!obj.contains("channel") || typeId(obj["channel"]) != QMetaType::QByteArray)
			return false;

		channel = obj["channel"].toByteArray();

		if(obj.contains("item-id"))
		{
			if(typeId(obj["item-id"]) != QMetaType::QByteArray)
				return false;

			itemId = obj["item-id"].toByteArray();
		}

		if(!obj.contains("count") || !canConvert(obj["count"], QMetaType::Int))
			return false;

		count = obj["count"].toInt();
		if(count < 0)
			return false;

		if(obj.contains("blocks"))
		{
			if(!canConvert(obj["blocks"], QMetaType::Int))
				return false;

			blocks = obj["blocks"].toInt();
		}

		if(!obj.contains("transport") || typeId(obj["transport"]) != QMetaType::QByteArray)
			return false;

		transport = obj["transport"].toByteArray();
	}
	else if(_type == "conn")
	{
		if(!obj.contains("id") || typeId(obj["id"]) != QMetaType::QByteArray)
			return false;

		connectionId = obj["id"].toByteArray();

		type = Connected;
		if(obj.contains("unavailable"))
		{
			if(typeId(obj["unavailable"]) != QMetaType::Bool)
				return false;

			if(obj["unavailable"].toBool())
				type = Disconnected;
		}

		if(type == Connected)
		{
			if(!obj.contains("type") || typeId(obj["type"]) != QMetaType::QByteArray)
				return false;

			QByteArray typeStr = obj["type"].toByteArray();
			if(typeStr == "ws")
				connectionType = WebSocket;
			else if(typeStr == "http")
				connectionType = Http;
			else
				return false;

			if(obj.contains("peer-address"))
			{
				if(typeId(obj["peer-address"]) != QMetaType::QByteArray)
					return false;

				QByteArray peerAddressStr = obj["peer-address"].toByteArray();
				if(!peerAddress.setAddress(QString::fromUtf8(peerAddressStr)))
					return false;
			}

			if(obj.contains("ssl"))
			{
				if(typeId(obj["ssl"]) != QMetaType::Bool)
					return false;

				ssl = obj["ssl"].toBool();
			}

			if(!obj.contains("ttl") || !canConvert(obj["ttl"], QMetaType::Int))
				return false;

			ttl = obj["ttl"].toInt();
			if(ttl < 0)
				return false;
		}
	}
	else if(_type == "sub")
	{
		if(!obj.contains("mode") || typeId(obj["mode"]) != QMetaType::QByteArray)
			return false;

		mode = obj["mode"].toByteArray();

		if(!obj.contains("channel") || typeId(obj["channel"]) != QMetaType::QByteArray)
			return false;

		channel = obj["channel"].toByteArray();

		type = Subscribed;
		if(obj.contains("unavailable"))
		{
			if(typeId(obj["unavailable"]) != QMetaType::Bool)
				return false;

			if(obj["unavailable"].toBool())
				type = Unsubscribed;
		}

		if(type == Subscribed)
		{
			if(!obj.contains("ttl") || !canConvert(obj["ttl"], QMetaType::Int))
				return false;

			ttl = obj["ttl"].toInt();
			if(ttl < 0)
				return false;

			if(obj.contains("subscribers"))
			{
				if(!canConvert(obj["subscribers"], QMetaType::Int))
					return false;

				subscribers = obj["subscribers"].toInt();
				if(subscribers < 0)
					return false;
			}
		}
	}
	else if(_type == "report")
	{
		type = Report;

		if(obj.contains("connections"))
		{
			if(!canConvert(obj["connections"], QMetaType::Int))
				return false;

			connectionsMax = obj["connections"].toInt();
		}

		if(obj.contains("minutes"))
		{
			if(!canConvert(obj["minutes"], QMetaType::Int))
				return false;

			connectionsMinutes = obj["minutes"].toInt();
		}

		if(obj.contains("received"))
		{
			if(!canConvert(obj["received"], QMetaType::Int))
				return false;

			messagesReceived = obj["received"].toInt();
		}

		if(obj.contains("sent"))
		{
			if(!canConvert(obj["sent"], QMetaType::Int))
				return false;

			messagesSent = obj["sent"].toInt();
		}

		if(obj.contains("http-response-sent"))
		{
			if(!canConvert(obj["http-response-sent"], QMetaType::Int))
				return false;

			httpResponseMessagesSent = obj["http-response-sent"].toInt();
		}

		if(obj.contains("blocks-received"))
		{
			if(!canConvert(obj["blocks-received"], QMetaType::Int))
				return false;

			blocksReceived = obj["blocks-received"].toInt();
		}

		if(obj.contains("blocks-sent"))
		{
			if(!canConvert(obj["blocks-sent"], QMetaType::Int))
				return false;

			blocksSent = obj["blocks-sent"].toInt();
		}

		if(obj.contains("duration"))
		{
			if(!canConvert(obj["duration"], QMetaType::Int))
				return false;

			duration = obj["duration"].toInt();
		}

		if(!tryGetInt(obj, "client-header-bytes-received", &clientHeaderBytesReceived))
			return false;
		if(!tryGetInt(obj, "client-header-bytes-sent", &clientHeaderBytesSent))
			return false;
		if(!tryGetInt(obj, "client-content-bytes-received", &clientContentBytesReceived))
			return false;
		if(!tryGetInt(obj, "client-content-bytes-sent", &clientContentBytesSent))
			return false;
		if(!tryGetInt(obj, "client-messages-received", &clientMessagesReceived))
			return false;
		if(!tryGetInt(obj, "client-messages-sent", &clientMessagesSent))
			return false;
		if(!tryGetInt(obj, "server-header-bytes-received", &serverHeaderBytesReceived))
			return false;
		if(!tryGetInt(obj, "server-header-bytes-sent", &serverHeaderBytesSent))
			return false;
		if(!tryGetInt(obj, "server-content-bytes-received", &serverContentBytesReceived))
			return false;
		if(!tryGetInt(obj, "server-content-bytes-sent", &serverContentBytesSent))
			return false;
		if(!tryGetInt(obj, "server-messages-received", &serverMessagesReceived))
			return false;
		if(!tryGetInt(obj, "server-messages-sent", &serverMessagesSent))
			return false;
	}
	else if(_type == "counts")
	{
		type = Counts;

		if(obj.contains("requests-received"))
		{
			if(!canConvert(obj["requests-received"], QMetaType::Int))
				return false;

			int x = obj["requests-received"].toInt();
			if(x < 0)
				return false;

			requestsReceived = x;
		}
	}
	else if(_type == "conn-max")
	{
		type = ConnectionsMax;

		if(!obj.contains("max") || !canConvert(obj["max"], QMetaType::Int))
			return false;

		int x = obj["max"].toInt();
		if(x < 0)
			return false;

		connectionsMax = x;

		if(!obj.contains("ttl") || !canConvert(obj["ttl"], QMetaType::Int))
			return false;

		x = obj["ttl"].toInt();
		if(x < 0)
			return false;

		ttl = x;

		if(obj.contains("retry-seq"))
		{
			if(!canConvert(obj["retry-seq"], QMetaType::LongLong))
				return false;

			int x = obj["retry-seq"].toLongLong();
			if(x < 0)
				return false;

			retrySeq = x;
		}
	}
	else
		return false;

	return true;
}
