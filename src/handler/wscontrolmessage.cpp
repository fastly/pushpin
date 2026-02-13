/*
 * Copyright (C) 2016-2019 Fanout, Inc.
 * Copyright (C) 2024 Fanout, Inc.
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

#include "wscontrolmessage.h"

#include <QVariant>
#include "qtcompat.h"
#include "variantutil.h"

using namespace VariantUtil;

WsControlMessage WsControlMessage::fromVariant(const QVariant &in, bool *ok, QString *errorMessage)
{
	QString pn = "grip control packet";

	if(!isKeyedObject(in))
	{
		setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
		return WsControlMessage();
	}

	pn = "grip control object";

	WsControlMessage out;

	bool ok_;
	QString type = getString(in, pn, "type", true, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return WsControlMessage();
	}

	if(type == "subscribe")
		out.type = Subscribe;
	else if(type == "unsubscribe")
		out.type = Unsubscribe;
	else if(type == "detach")
		out.type = Detach;
	else if(type == "session")
		out.type = Session;
	else if(type == "set-meta")
		out.type = SetMeta;
	else if(type == "keep-alive")
		out.type = KeepAlive;
	else if(type == "send-delayed")
		out.type = SendDelayed;
	else if(type == "flush-delayed")
		out.type = FlushDelayed;
	else
	{
		setError(ok, errorMessage, QString("'type' contains unknown value: %1").arg(type));
		return WsControlMessage();
	}

	if(out.type == Subscribe || out.type == Unsubscribe)
	{
		out.channel = getString(in, pn, "channel", true, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlMessage();
		}

		if(out.channel.isEmpty())
		{
			setError(ok, errorMessage, QString("%1 contains 'channel' with invalid value").arg(pn));
			return WsControlMessage();
		}

		if(out.type == Subscribe)
		{
			QVariantList vfilters = getList(in, pn, "filters", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return WsControlMessage();
			}

			foreach(const QVariant &vfilter, vfilters)
			{
				QString filter = getString(vfilter, &ok_);
				if(!ok_)
				{
					setError(ok, errorMessage, "filters contains value with wrong type");
					return WsControlMessage();
				}

				out.filters += filter;
			}
		}
	}
	else if(out.type == Session)
	{
		out.sessionId = getString(in, pn, "id", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlMessage();
		}
	}
	else if(out.type == SetMeta)
	{
		out.metaName = getString(in, pn, "name", true, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlMessage();
		}

		if(out.metaName.isEmpty())
		{
			setError(ok, errorMessage, QString("%1 contains 'name' with invalid value").arg(pn));
			return WsControlMessage();
		}

		out.metaValue = getString(in, pn, "value", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlMessage();
		}
	}
	else if(out.type == KeepAlive || out.type == SendDelayed)
	{
		QString typeStr = getString(in, pn, "message-type", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return WsControlMessage();
		}

		if(!typeStr.isNull())
		{
			if(typeStr == "text")
				out.messageType = Text;
			else if(typeStr == "binary")
				out.messageType = Binary;
			else if(typeStr == "ping")
				out.messageType = Ping;
			else if(typeStr == "pong")
				out.messageType = Pong;
			else
			{
				setError(ok, errorMessage, QString("%1 contains 'message-type' with unknown value").arg(pn));
				return WsControlMessage();
			}
		}
		else
		{
			// Default
			out.messageType = Text;
		}

		if(keyedObjectContains(in, "content-bin"))
		{
			QVariant vcontentBin = keyedObjectGetValue(in, "content-bin");

			if(typeId(in) == QMetaType::QVariantMap) // JSON input
			{
				if(typeId(vcontentBin) != QMetaType::QString)
				{
					setError(ok, errorMessage, QString("%1 contains 'content-bin' with wrong type").arg(pn));
					return WsControlMessage();
				}

				out.content = QByteArray::fromBase64(vcontentBin.toString().toUtf8());
			}
			else
			{
				if(typeId(vcontentBin) != QMetaType::QByteArray)
				{
					setError(ok, errorMessage, QString("%1 contains 'content-bin' with wrong type").arg(pn));
					return WsControlMessage();
				}

				out.content = vcontentBin.toByteArray();
			}

			if(((int)out.messageType) == -1)
				out.messageType = Binary;
		}
		else if(keyedObjectContains(in, "content"))
		{
			QVariant vcontent = keyedObjectGetValue(in, "content");
			if(typeId(vcontent) == QMetaType::QByteArray)
				out.content = vcontent.toByteArray();
			else if(typeId(vcontent) == QMetaType::QString)
				out.content = vcontent.toString().toUtf8();
			else
			{
				setError(ok, errorMessage, QString("%1 contains 'content' with wrong type").arg(pn));
				return WsControlMessage();
			}

			if(((int)out.messageType) == -1)
				out.messageType = Text;
		}

		if(!out.content.isNull())
		{
			if(keyedObjectContains(in, "timeout"))
			{
				QVariant vtimeout = keyedObjectGetValue(in, "timeout");
				if(!canConvert(vtimeout, QMetaType::Int))
				{
					setError(ok, errorMessage, QString("%1 contains 'timeout' with wrong type").arg(pn));
					return WsControlMessage();
				}

				out.timeout = vtimeout.toInt();

				if(out.timeout < 0)
				{
					setError(ok, errorMessage, QString("%1 contains 'timeout' with invalid value").arg(pn));
					return WsControlMessage();
				}
			}
		}

		if(out.type == KeepAlive)
		{
			QString mode = getString(in, pn, "mode", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return WsControlMessage();
			}

			if(!mode.isNull())
				out.keepAliveMode = mode.toUtf8();
		}
	}

	return out;
}
