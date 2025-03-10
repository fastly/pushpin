/*
 * Copyright (C) 2014-2022 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#include "wscontrolpacket.h"

#include <assert.h>
#include "qtcompat.h"

// FIXME: rewrite packet class using this code?
/*class WsControlPacket
{
public:
	class Message
	{
	public:
		enum Type
		{
			Here,
			Gone,
			Cancel,
			Grip
		};

		Type type;
		QString cid;
		QString channelPrefix; // here only
		QByteArray message; // grip only
	};

	QString channelPrefix;
	QList<Message> messages;

	static WsControlPacket fromVariant(const QVariant &in, bool *ok = 0, QString *errorMessage = 0)
	{
		QString pn = "wscontrol packet";

		if(!isKeyedObject(in))
		{
			setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
			return WsControlPacket();
		}

		pn = "wscontrol object";

		bool ok_;
		QVariantList vitems = getList(in, pn, "items", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlPacket();
		}

		WsControlPacket out;

		foreach(const QVariant &vitem, vitems)
		{
			Message msg;

			pn = "wscontrol item";

			QString type = getString(vitem, pn, "type", true, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return WsControlPacket();
			}

			if(type == "here")
				msg.type = Message::Here;
			else if(type == "gone")
				msg.type = Message::Gone;
			else if(type == "cancel")
				msg.type = Message::Cancel;
			else if(type == "grip")
				msg.type = Message::Grip;
			else
			{
				setError(ok, errorMessage, QString("'type' contains unknown value: %1").arg(type));
				return WsControlPacket();
			}

			msg.cid = getString(vitem, pn, "cid", true, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return WsControlPacket();
			}

			msg.uri = QUrl::fromEncoded(getString(vitem, pn, "uri", false, &ok_, errorMessage).toUtf8(), QUrl::StrictMode);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return WsControlPacket();
			}

			msg.channelPrefix = getString(vitem, pn, "channel-prefix", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return WsControlPacket();
			}

			if(msg.type == Message::Grip)
			{
				if(!keyedObjectContains(vitem, "message"))
				{
					setError(ok, errorMessage, QString("'%1' does not contain 'message'").arg(pn));
					return WsControlPacket();
				}

				QVariant vmessage = keyedObjectGetValue(vitem, "message");
				if(vmessage.type() != QVariant::ByteArray)
				{
					setError(ok, errorMessage, QString("'%1' contains 'message' with wrong type").arg(pn));
					return WsControlPacket();
				}

				msg.message = vmessage.toByteArray();
			}

			out.messages += msg;
		}

		setSuccess(ok, errorMessage);
		return out;
	}
};*/

QVariant WsControlPacket::toVariant() const
{
	QVariantHash obj;

	obj["from"] = from;

	QVariantList vitems;
	foreach(const Item &item, items)
	{
		QVariantHash vitem;

		vitem["cid"] = item.cid;

		QByteArray typeStr;
		switch(item.type)
		{
			case Item::Here:           typeStr = "here"; break;
			case Item::KeepAlive:      typeStr = "keep-alive"; break;
			case Item::Gone:           typeStr = "gone"; break;
			case Item::Grip:           typeStr = "grip"; break;
			case Item::KeepAliveSetup: typeStr = "keep-alive-setup"; break;
			case Item::Cancel:         typeStr = "cancel"; break;
			case Item::Send:           typeStr = "send"; break;
			case Item::NeedKeepAlive:  typeStr = "need-keep-alive"; break;
			case Item::Subscribe:      typeStr = "subscribe"; break;
			case Item::Refresh:        typeStr = "refresh"; break;
			case Item::Close:          typeStr = "close"; break;
			case Item::Detach:         typeStr = "detach"; break;
			case Item::Ack:            typeStr = "ack"; break;
			default:
				assert(0);
		}
		vitem["type"] = typeStr;

		if(!item.requestId.isEmpty())
			vitem["req-id"] = item.requestId;

		if(!item.uri.isEmpty())
			vitem["uri"] = item.uri.toEncoded();

		if(!item.contentType.isEmpty())
			vitem["content-type"] = item.contentType;

		if(!item.message.isNull())
			vitem["message"] = item.message;

		if(item.queue)
			vitem["queue"] = true;

		if(item.code >= 0)
			vitem["code"] = item.code;

		if(!item.reason.isEmpty())
			vitem["reason"] = item.reason;

		if(item.debug)
			vitem["debug"] = true;

		if(!item.route.isEmpty())
			vitem["route"] = item.route;

		if(item.separateStats)
			vitem["separate-stats"] = true;

		if(!item.channelPrefix.isEmpty())
			vitem["channel-prefix"] = item.channelPrefix;

		if(item.logLevel >= 0)
			vitem["log-level"] = item.logLevel;

		if(item.trusted)
			vitem["trusted"] = true;

		if(!item.channel.isEmpty())
			vitem["channel"] = item.channel;

		if(item.ttl >= 0)
			vitem["ttl"] = item.ttl;

		if(item.timeout >= 0)
			vitem["timeout"] = item.timeout;

		if(!item.keepAliveMode.isEmpty())
			vitem["keep-alive-mode"] = item.keepAliveMode;

		vitems += vitem;
	}

	obj["items"] = vitems;
	return obj;
}

bool WsControlPacket::fromVariant(const QVariant &in)
{
	if(typeId(in) != QMetaType::QVariantHash)
		return false;

	QVariantHash obj = in.toHash();

	if(!obj.contains("from") || typeId(obj["from"]) != QMetaType::QByteArray)
		return false;

	from = obj["from"].toByteArray();

	if(!obj.contains("items") || typeId(obj["items"]) != QMetaType::QVariantList)
		return false;

	QVariantList vitems = obj["items"].toList();

	items.clear();
	foreach(const QVariant &v, vitems)
	{
		if(typeId(v) != QMetaType::QVariantHash)
			return false;

		QVariantHash vitem = v.toHash();

		Item item;

		if(!vitem.contains("cid") || typeId(vitem["cid"]) != QMetaType::QByteArray)
			return false;
		item.cid = vitem["cid"].toByteArray();

		if(!vitem.contains("type") || typeId(vitem["type"]) != QMetaType::QByteArray)
			return false;
		QByteArray typeStr = vitem["type"].toByteArray();

		if(typeStr == "here")
			item.type = Item::Here;
		else if(typeStr == "keep-alive")
			item.type = Item::KeepAlive;
		else if(typeStr == "gone")
			item.type = Item::Gone;
		else if(typeStr == "grip")
			item.type = Item::Grip;
		else if(typeStr == "keep-alive-setup")
			item.type = Item::KeepAliveSetup;
		else if(typeStr == "cancel")
			item.type = Item::Cancel;
		else if(typeStr == "send")
			item.type = Item::Send;
		else if(typeStr == "need-keep-alive")
			item.type = Item::NeedKeepAlive;
		else if(typeStr == "subscribe")
			item.type = Item::Subscribe;
		else if(typeStr == "refresh")
			item.type = Item::Refresh;
		else if(typeStr == "close")
			item.type = Item::Close;
		else if(typeStr == "detach")
			item.type = Item::Detach;
		else if(typeStr == "ack")
			item.type = Item::Ack;
		else
			return false;

		if(vitem.contains("req-id"))
		{
			if(typeId(vitem["req-id"]) != QMetaType::QByteArray)
				return false;

			item.requestId = vitem["req-id"].toByteArray();
		}

		if(vitem.contains("uri"))
		{
			if(typeId(vitem["uri"]) != QMetaType::QByteArray)
				return false;

			item.uri = QUrl::fromEncoded(vitem["uri"].toByteArray(), QUrl::StrictMode);
		}

		if(vitem.contains("content-type"))
		{
			if(typeId(vitem["content-type"]) != QMetaType::QByteArray)
				return false;

			QByteArray contentType = vitem["content-type"].toByteArray();
			if(!contentType.isEmpty())
				item.contentType = contentType;
		}

		if(vitem.contains("message"))
		{
			if(typeId(vitem["message"]) != QMetaType::QByteArray)
				return false;

			item.message = vitem["message"].toByteArray();
		}

		if(vitem.contains("queue"))
		{
			if(typeId(vitem["queue"]) != QMetaType::Bool)
				return false;

			item.queue = vitem["queue"].toBool();
		}

		if(vitem.contains("code"))
		{
			if(!canConvert(vitem["code"], QMetaType::Int))
				return false;

			item.code = vitem["code"].toInt();
		}

		if(vitem.contains("reason"))
		{
			if(typeId(vitem["reason"]) != QMetaType::QByteArray)
				return false;

			item.reason = vitem["reason"].toByteArray();
		}

		if(vitem.contains("debug"))
		{
			if(typeId(vitem["debug"]) != QMetaType::Bool)
				return false;

			item.debug = vitem["debug"].toBool();
		}

		if(vitem.contains("route"))
		{
			if(typeId(vitem["route"]) != QMetaType::QByteArray)
				return false;

			QByteArray route = vitem["route"].toByteArray();
			if(!route.isEmpty())
				item.route = route;
		}

		if(vitem.contains("separate-stats"))
		{
			if(typeId(vitem["separate-stats"]) != QMetaType::Bool)
				return false;

			item.separateStats = vitem["separate-stats"].toBool();
		}

		if(vitem.contains("channel-prefix"))
		{
			if(typeId(vitem["channel-prefix"]) != QMetaType::QByteArray)
				return false;

			QByteArray channelPrefix = vitem["channel-prefix"].toByteArray();
			if(!channelPrefix.isEmpty())
				item.channelPrefix = channelPrefix;
		}

		if(vitem.contains("log-level"))
		{
			if(!canConvert(vitem["log-level"], QMetaType::Int))
				return false;

			item.logLevel = vitem["log-level"].toInt();
		}

		if(vitem.contains("trusted"))
		{
			if(typeId(vitem["trusted"]) != QMetaType::Bool)
				return false;

			item.trusted = vitem["trusted"].toBool();
		}

		if(vitem.contains("channel"))
		{
			if(typeId(vitem["channel"]) != QMetaType::QByteArray)
				return false;

			QByteArray channel = vitem["channel"].toByteArray();
			if(!channel.isEmpty())
				item.channel = channel;
		}

		if(vitem.contains("ttl"))
		{
			if(!canConvert(vitem["ttl"], QMetaType::Int))
				return false;

			item.ttl = vitem["ttl"].toInt();
			if(item.ttl < 0)
				item.ttl = 0;
		}

		if(vitem.contains("timeout"))
		{
			if(!canConvert(vitem["timeout"], QMetaType::Int))
				return false;

			item.timeout = vitem["timeout"].toInt();
			if(item.timeout < 0)
				item.timeout = 0;
		}

		if(vitem.contains("keep-alive-mode"))
		{
			if(!canConvert(vitem["keep-alive-mode"], QMetaType::QByteArray))
				return false;

			QByteArray keepAliveMode = vitem["keep-alive-mode"].toByteArray();
			if(!keepAliveMode.isEmpty())
				item.keepAliveMode = keepAliveMode;
		}

		items += item;
	}

	return true;
}
