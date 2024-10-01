/*
 * Copyright (C) 2012-2016 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include "zhttprequestpacket.h"

#include <stdio.h>
#include "qtcompat.h"
#include "tnetstring.h"

QVariant ZhttpRequestPacket::toVariant() const
{
	QVariantHash obj;

	if(!from.isEmpty())
		obj["from"] = from;

	if(!ids.isEmpty())
	{
		if(ids.count() == 1)
		{
			const Id &id = ids.first();
			if(!id.id.isEmpty())
				obj["id"] = id.id;
			if(id.seq != -1)
				obj["seq"] = id.seq;
		}
		else
		{
			QVariantList vl;
			foreach(const Id &id, ids)
			{
				QVariantHash vh;
				if(!id.id.isEmpty())
					vh["id"] = id.id;
				if(id.seq != -1)
					vh["seq"] = id.seq;
				vl += vh;
			}
			obj["id"] = vl;
		}
	}

	QByteArray typeStr;
	switch(type)
	{
		case Error:          typeStr = "error"; break;
		case Credit:         typeStr = "credit"; break;
		case KeepAlive:      typeStr = "keep-alive"; break;
		case Cancel:         typeStr = "cancel"; break;
		case HandoffStart:   typeStr = "handoff-start"; break;
		case HandoffProceed: typeStr = "handoff-proceed"; break;
		case Close:          typeStr = "close"; break;
		case Ping:           typeStr = "ping"; break;
		case Pong:           typeStr = "pong"; break;
		default: break;
	}

	if(!typeStr.isEmpty())
		obj["type"] = typeStr;

	if(type == Error && !condition.isEmpty())
		obj["condition"] = condition;

	if(credits != -1)
		obj["credits"] = credits;

	if(more)
		obj["more"] = true;

	if(stream)
		obj["stream"] = true;

	if(routerResp)
		obj["router-resp"] = true;

	if(maxSize != -1)
		obj["max-size"] = maxSize;

	if(timeout != -1)
		obj["timeout"] = timeout;

	if(!method.isEmpty())
		obj["method"] = method.toLatin1();

	if(!uri.isEmpty())
		obj["uri"] = uri.toEncoded();

	if(!headers.isEmpty())
	{
		QVariantList vheaders;
		foreach(const HttpHeader &h, headers)
		{
			QVariantList vheader;
			vheader += h.first;
			vheader += h.second;
			vheaders += QVariant(vheader);
		}

		obj["headers"] = vheaders;
	}

	if(!body.isNull())
		obj["body"] = body;

	if(!contentType.isEmpty())
		obj["content-type"] = contentType;

	if(code != -1)
		obj["code"] = code;

	if(userData.isValid())
		obj["user-data"] = userData;

	if(!peerAddress.isNull())
		obj["peer-address"] = peerAddress.toString().toUtf8();

	if(peerPort != -1)
		obj["peer-port"] = QByteArray::number(peerPort);

	if(!connectHost.isEmpty())
		obj["connect-host"] = connectHost.toUtf8();

	if(connectPort != -1)
		obj["connect-port"] = connectPort;

	if(ignorePolicies)
		obj["ignore-policies"] = true;

	if(trustConnectHost)
		obj["trust-connect-host"] = true;

	if(ignoreTlsErrors)
		obj["ignore-tls-errors"] = true;

	if(followRedirects)
		obj["follow-redirects"] = true;

	if(passthrough.isValid())
		obj["passthrough"] = passthrough;

	if(multi || quiet)
	{
		QVariantHash ext;

		if(multi)
			ext["multi"] = true;

		if(quiet)
			ext["quiet"] = true;

		obj["ext"] = ext;
	}

	return obj;
}

bool ZhttpRequestPacket::fromVariant(const QVariant &in)
{
	if(typeId(in) != QMetaType::QVariantHash)
		return false;

	QVariantHash obj = in.toHash();

	from.clear();
	if(obj.contains("from"))
	{
		if(typeId(obj["from"]) != QMetaType::QByteArray)
			return false;

		from = obj["from"].toByteArray();
	}

	ids.clear();
	if(obj.contains("id"))
	{
		if(typeId(obj["id"]) == QMetaType::QByteArray)
		{
			Id id;
			id.id = obj["id"].toByteArray();
			ids += id;
		}
		else if(typeId(obj["id"]) == QMetaType::QVariantList)
		{
			QVariantList vl = obj["id"].toList();
			foreach(const QVariant &v, vl)
			{
				if(typeId(v) != QMetaType::QVariantHash)
					return false;

				Id id;

				QVariantHash vh = v.toHash();

				if(vh.contains("id"))
				{
					if(typeId(vh["id"]) != QMetaType::QByteArray)
						return false;

					id.id = vh["id"].toByteArray();
				}

				if(vh.contains("seq"))
				{
					if(!canConvert(vh["seq"], QMetaType::Int))
						return false;

					id.seq = vh["seq"].toInt();
				}

				ids += id;
			}
		}
		else
			return false;
	}

	if(obj.contains("seq"))
	{
		if(!canConvert(obj["seq"], QMetaType::Int))
			return false;

		if(ids.isEmpty())
			ids += Id();

		ids.first().seq = obj["seq"].toInt();
	}

	type = Data;
	if(obj.contains("type"))
	{
		if(typeId(obj["type"]) != QMetaType::QByteArray)
			return false;

		QByteArray typeStr = obj["type"].toByteArray();

		if(typeStr == "error")
			type = Error;
		else if(typeStr == "credit")
			type = Credit;
		else if(typeStr == "keep-alive")
			type = KeepAlive;
		else if(typeStr == "cancel")
			type = Cancel;
		else if(typeStr == "handoff-start")
			type = HandoffStart;
		else if(typeStr == "handoff-proceed")
			type = HandoffProceed;
		else if(typeStr == "close")
			type = Close;
		else if(typeStr == "ping")
			type = Ping;
		else if(typeStr == "pong")
			type = Pong;
		else
			return false;
	}

	if(type == Error)
	{
		condition.clear();
		if(obj.contains("condition"))
		{
			if(typeId(obj["condition"]) != QMetaType::QByteArray)
				return false;

			condition = obj["condition"].toByteArray();
		}
	}

	credits = -1;
	if(obj.contains("credits"))
	{
		if(!canConvert(obj["credits"], QMetaType::Int))
			return false;

		credits = obj["credits"].toInt();
	}

	more = false;
	if(obj.contains("more"))
	{
		if(typeId(obj["more"]) != QMetaType::Bool)
			return false;

		more = obj["more"].toBool();
	}

	stream = false;
	if(obj.contains("stream"))
	{
		if(typeId(obj["stream"]) != QMetaType::Bool)
			return false;

		stream = obj["stream"].toBool();
	}

	routerResp = false;
	if(obj.contains("router-resp"))
	{
		if(typeId(obj["router-resp"]) != QMetaType::Bool)
			return false;

		routerResp = obj["router-resp"].toBool();
	}

	maxSize = -1;
	if(obj.contains("max-size"))
	{
		if(!canConvert(obj["max-size"], QMetaType::Int))
			return false;

		maxSize = obj["max-size"].toInt();
	}

	timeout = -1;
	if(obj.contains("timeout"))
	{
		if(!canConvert(obj["timeout"], QMetaType::Int))
			return false;

		timeout = obj["timeout"].toInt();
	}

	method.clear();
	if(obj.contains("method"))
	{
		if(typeId(obj["method"]) != QMetaType::QByteArray)
			return false;

		method = QString::fromLatin1(obj["method"].toByteArray());
	}

	uri.clear();
	if(obj.contains("uri"))
	{
		if(typeId(obj["uri"]) != QMetaType::QByteArray)
			return false;

		uri = QUrl::fromEncoded(obj["uri"].toByteArray(), QUrl::StrictMode);
	}

	headers.clear();
	if(obj.contains("headers"))
	{
		if(typeId(obj["headers"]) != QMetaType::QVariantList)
			return false;

		foreach(const QVariant &i, obj["headers"].toList())
		{
			QVariantList list = i.toList();
			if(list.count() != 2)
				return false;

			if(typeId(list[0]) != QMetaType::QByteArray || typeId(list[1]) != QMetaType::QByteArray)
				return false;

			headers += HttpHeader(list[0].toByteArray(), list[1].toByteArray());
		}
	}

	body.clear();
	if(obj.contains("body"))
	{
		if(typeId(obj["body"]) != QMetaType::QByteArray)
			return false;

		body = obj["body"].toByteArray();
	}

	contentType.clear();
	if(obj.contains("content-type"))
	{
		if(typeId(obj["content-type"]) != QMetaType::QByteArray)
			return false;

		contentType = obj["content-type"].toByteArray();
	}

	code = -1;
	if(obj.contains("code"))
	{
		if(!canConvert(obj["code"], QMetaType::Int))
			return false;

		code = obj["code"].toInt();
	}

	userData = obj.value("user-data");

	peerAddress = QHostAddress();
	if(obj.contains("peer-address"))
	{
		if(typeId(obj["peer-address"]) != QMetaType::QByteArray)
			return false;

		peerAddress = QHostAddress(QString::fromUtf8(obj["peer-address"].toByteArray()));
	}

	peerPort = -1;
	if(obj.contains("peer-port"))
	{
		if(!canConvert(obj["peer-port"], QMetaType::Int))
			return false;

		peerPort = obj["peer-port"].toInt();
	}

	connectHost.clear();
	if(obj.contains("connect-host"))
	{
		if(typeId(obj["connect-host"]) != QMetaType::QByteArray)
			return false;

		connectHost = QString::fromUtf8(obj["connect-host"].toByteArray());
	}

	connectPort = -1;
	if(obj.contains("connect-port"))
	{
		if(!canConvert(obj["connect-port"], QMetaType::Int))
			return false;

		connectPort = obj["connect-port"].toInt();
	}

	ignorePolicies = false;
	if(obj.contains("ignore-policies"))
	{
		if(typeId(obj["ignore-policies"]) != QMetaType::Bool)
			return false;

		ignorePolicies = obj["ignore-policies"].toBool();
	}

	trustConnectHost = false;
	if(obj.contains("trust-connect-host"))
	{
		if(typeId(obj["trust-connect-host"]) != QMetaType::Bool)
			return false;

		trustConnectHost = obj["trust-connect-host"].toBool();
	}

	ignoreTlsErrors = false;
	if(obj.contains("ignore-tls-errors"))
	{
		if(typeId(obj["ignore-tls-errors"]) != QMetaType::Bool)
			return false;

		ignoreTlsErrors = obj["ignore-tls-errors"].toBool();
	}

	followRedirects = false;
	if(obj.contains("follow-redirects"))
	{
		if(typeId(obj["follow-redirects"]) != QMetaType::Bool)
			return false;

		followRedirects = obj["follow-redirects"].toBool();
	}

	passthrough = obj.value("passthrough");

	multi = false;
	if(obj.contains("ext"))
	{
		if(typeId(obj["ext"]) != QMetaType::QVariantHash)
			return false;

		QVariantHash ext = obj["ext"].toHash();
		if(ext.contains("multi") && typeId(ext["multi"]) == QMetaType::Bool)
		{
			multi = ext["multi"].toBool();
		}

		if(ext.contains("quiet") && typeId(ext["quiet"]) == QMetaType::Bool)
		{
			quiet = ext["quiet"].toBool();
		}
	}

	return true;
}
