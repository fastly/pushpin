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

#include "wscontrolpacket.h"

#include <assert.h>

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

		if(!item.route.isEmpty())
			vitem["route"] = item.route;

		if(item.separateStats)
			vitem["separate-stats"] = true;

		if(!item.channelPrefix.isEmpty())
			vitem["channel-prefix"] = item.channelPrefix;

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
	if(in.type() != QVariant::Hash)
		return false;

	QVariantHash obj = in.toHash();

	if(!obj.contains("items") || obj["items"].type() != QVariant::List)
		return false;
	QVariantList vitems = obj["items"].toList();

	items.clear();
	foreach(const QVariant &v, vitems)
	{
		if(v.type() != QVariant::Hash)
			return false;

		QVariantHash vitem = v.toHash();

		Item item;

		if(!vitem.contains("cid") || vitem["cid"].type() != QVariant::ByteArray)
			return false;
		item.cid = vitem["cid"].toByteArray();

		if(!vitem.contains("type") || vitem["type"].type() != QVariant::ByteArray)
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
			if(vitem["req-id"].type() != QVariant::ByteArray)
				return false;

			item.requestId = vitem["req-id"].toByteArray();
		}

		if(vitem.contains("uri"))
		{
			if(vitem["uri"].type() != QVariant::ByteArray)
				return false;

			item.uri = QUrl::fromEncoded(vitem["uri"].toByteArray(), QUrl::StrictMode);
		}

		if(vitem.contains("content-type"))
		{
			if(vitem["content-type"].type() != QVariant::ByteArray)
				return false;

			QByteArray contentType = vitem["content-type"].toByteArray();
			if(!contentType.isEmpty())
				item.contentType = contentType;
		}

		if(vitem.contains("message"))
		{
			if(vitem["message"].type() != QVariant::ByteArray)
				return false;

			item.message = vitem["message"].toByteArray();
		}

		if(vitem.contains("queue"))
		{
			if(vitem["queue"].type() != QVariant::Bool)
				return false;

			item.queue = vitem["queue"].toBool();
		}

		if(vitem.contains("code"))
		{
			if(!vitem["code"].canConvert(QVariant::Int))
				return false;

			item.code = vitem["code"].toInt();
		}

		if(vitem.contains("reason"))
		{
			if(vitem["reason"].type() != QVariant::ByteArray)
				return false;

			item.reason = vitem["reason"].toByteArray();
		}

		if(vitem.contains("route"))
		{
			if(vitem["route"].type() != QVariant::ByteArray)
				return false;

			QByteArray route = vitem["route"].toByteArray();
			if(!route.isEmpty())
				item.route = route;
		}

		if(vitem.contains("separate-stats"))
		{
			if(vitem["separate-stats"].type() != QVariant::Bool)
				return false;

			item.separateStats = vitem["separate-stats"].toBool();
		}

		if(vitem.contains("channel-prefix"))
		{
			if(vitem["channel-prefix"].type() != QVariant::ByteArray)
				return false;

			QByteArray channelPrefix = vitem["channel-prefix"].toByteArray();
			if(!channelPrefix.isEmpty())
				item.channelPrefix = channelPrefix;
		}

		if(vitem.contains("channel"))
		{
			if(vitem["channel"].type() != QVariant::ByteArray)
				return false;

			QByteArray channel = vitem["channel"].toByteArray();
			if(!channel.isEmpty())
				item.channel = channel;
		}

		if(vitem.contains("ttl"))
		{
			if(!vitem["ttl"].canConvert(QVariant::Int))
				return false;

			item.ttl = vitem["ttl"].toInt();
			if(item.ttl < 0)
				item.ttl = 0;
		}

		if(vitem.contains("timeout"))
		{
			if(!vitem["timeout"].canConvert(QVariant::Int))
				return false;

			item.timeout = vitem["timeout"].toInt();
			if(item.timeout < 0)
				item.timeout = 0;
		}

		if(vitem.contains("keep-alive-mode"))
		{
			if(!vitem["keep-alive-mode"].canConvert(QVariant::ByteArray))
				return false;

			QByteArray keepAliveMode = vitem["keep-alive-mode"].toByteArray();
			if(!keepAliveMode.isEmpty())
				item.keepAliveMode = keepAliveMode;
		}

		items += item;
	}

	return true;
}
