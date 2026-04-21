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
#include "variant.h"
#include "tnetstring.h"

Variant ZhttpRequestPacket::toVariant() const
{
	VariantHash obj;

	if(!from.isEmpty())
		obj["from"] = from.asQByteArray();

	if(!ids.isEmpty())
	{
		if(ids.count() == 1)
		{
			const Id &id = ids.first();
			if(!id.id.isEmpty())
				obj["id"] = id.id.asQByteArray();
			if(id.seq != -1)
				obj["seq"] = id.seq;
		}
		else
		{
			VariantList vl;
			foreach(const Id &id, ids)
			{
				VariantHash vh;
				if(!id.id.isEmpty())
					vh["id"] = id.id.asQByteArray();
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
		obj["condition"] = condition.asQByteArray();

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
		obj["method"] = method.toUtf8().asQByteArray();

	if(!uri.isEmpty())
		obj["uri"] = uri.toEncoded();

	if(!headers.isEmpty())
	{
		VariantList vheaders;
		foreach(const HttpHeader &h, headers)
		{
			VariantList vheader;
			vheader += h.first.asQByteArray();
			vheader += h.second.asQByteArray();
			vheaders += Variant(vheader);
		}

		obj["headers"] = vheaders;
	}

	if(!body.isNull())
		obj["body"] = body.asQByteArray();

	if(!contentType.isEmpty())
		obj["content-type"] = contentType.asQByteArray();

	if(code != -1)
		obj["code"] = code;

	if(userData.isValid())
		obj["user-data"] = userData;

	if(!peerAddress.isNull())
		obj["peer-address"] = peerAddress.toString().toUtf8();

	if(peerPort != -1)
		obj["peer-port"] = QByteArray::number(peerPort);

	if(!connectHost.isEmpty())
		obj["connect-host"] = connectHost.toUtf8().asQByteArray();

	if(connectPort != -1)
		obj["connect-port"] = connectPort;

	if(ignorePolicies)
		obj["ignore-policies"] = true;

	if(trustConnectHost)
		obj["trust-connect-host"] = true;

	if(ignoreTlsErrors)
		obj["ignore-tls-errors"] = true;

	if(!clientCert.isEmpty())
		obj["client-cert"] = clientCert.toUtf8().asQByteArray();

	if(!clientKey.isEmpty())
		obj["client-key"] = clientKey.toUtf8().asQByteArray();

	if(followRedirects)
		obj["follow-redirects"] = true;

	if(passthrough.isValid())
		obj["passthrough"] = passthrough;

	if(multi || quiet)
	{
		VariantHash ext;

		if(multi)
			ext["multi"] = true;

		if(quiet)
			ext["quiet"] = true;

		obj["ext"] = ext;
	}

	return obj;
}

bool ZhttpRequestPacket::fromVariant(const Variant &in)
{
	if(typeId(in) != VariantType::Hash)
		return false;

	VariantHash obj = in.toHash();

	from.clear();
	if(obj.contains("from"))
	{
		if(typeId(obj["from"]) != VariantType::ByteArray)
			return false;

		from = obj["from"].toByteArray();
	}

	ids.clear();
	if(obj.contains("id"))
	{
		if(typeId(obj["id"]) == VariantType::ByteArray)
		{
			Id id;
			id.id = obj["id"].toByteArray();
			ids += id;
		}
		else if(typeId(obj["id"]) == VariantType::List)
		{
			VariantList vl = obj["id"].toList();
			for(const Variant &v : vl)
			{
				if(typeId(v) != VariantType::Hash)
					return false;

				Id id;

				VariantHash vh = v.toHash();

				if(vh.contains("id"))
				{
					if(typeId(vh["id"]) != VariantType::ByteArray)
						return false;

					id.id = vh["id"].toByteArray();
				}

				if(vh.contains("seq"))
				{
					if(!canConvert(vh["seq"], VariantType::Int))
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
		if(!canConvert(obj["seq"], VariantType::Int))
			return false;

		if(ids.isEmpty())
			ids += Id();

		ids.first().seq = obj["seq"].toInt();
	}

	type = Data;
	if(obj.contains("type"))
	{
		if(typeId(obj["type"]) != VariantType::ByteArray)
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
			if(typeId(obj["condition"]) != VariantType::ByteArray)
				return false;

			condition = obj["condition"].toByteArray();
		}
	}

	credits = -1;
	if(obj.contains("credits"))
	{
		if(!canConvert(obj["credits"], VariantType::Int))
			return false;

		credits = obj["credits"].toInt();
	}

	more = false;
	if(obj.contains("more"))
	{
		if(typeId(obj["more"]) != VariantType::Bool)
			return false;

		more = obj["more"].toBool();
	}

	stream = false;
	if(obj.contains("stream"))
	{
		if(typeId(obj["stream"]) != VariantType::Bool)
			return false;

		stream = obj["stream"].toBool();
	}

	routerResp = false;
	if(obj.contains("router-resp"))
	{
		if(typeId(obj["router-resp"]) != VariantType::Bool)
			return false;

		routerResp = obj["router-resp"].toBool();
	}

	maxSize = -1;
	if(obj.contains("max-size"))
	{
		if(!canConvert(obj["max-size"], VariantType::Int))
			return false;

		maxSize = obj["max-size"].toInt();
	}

	timeout = -1;
	if(obj.contains("timeout"))
	{
		if(!canConvert(obj["timeout"], VariantType::Int))
			return false;

		timeout = obj["timeout"].toInt();
	}

	method.clear();
	if(obj.contains("method"))
	{
		if(typeId(obj["method"]) != VariantType::ByteArray)
			return false;

		method = QString::fromLatin1(obj["method"].toByteArray());
	}

	uri.clear();
	if(obj.contains("uri"))
	{
		if(typeId(obj["uri"]) != VariantType::ByteArray)
			return false;

		uri = Url::fromEncoded(obj["uri"].toByteArray(), Url::StrictMode);
	}

	headers.clear();
	if(obj.contains("headers"))
	{
		if(typeId(obj["headers"]) != VariantType::List)
			return false;

		for(const Variant &i : obj["headers"].toList())
		{
			VariantList list = i.toList();
			if(list.count() != 2)
				return false;

			if(typeId(list[0]) != VariantType::ByteArray || typeId(list[1]) != VariantType::ByteArray)
				return false;

			headers += HttpHeader(list[0].toByteArray(), list[1].toByteArray());
		}
	}

	body.clear();
	if(obj.contains("body"))
	{
		if(typeId(obj["body"]) != VariantType::ByteArray)
			return false;

		body = obj["body"].toByteArray();
	}

	contentType.clear();
	if(obj.contains("content-type"))
	{
		if(typeId(obj["content-type"]) != VariantType::ByteArray)
			return false;

		contentType = obj["content-type"].toByteArray();
	}

	code = -1;
	if(obj.contains("code"))
	{
		if(!canConvert(obj["code"], VariantType::Int))
			return false;

		code = obj["code"].toInt();
	}

	userData = obj.value("user-data");

	peerAddress = QHostAddress();
	if(obj.contains("peer-address"))
	{
		if(typeId(obj["peer-address"]) != VariantType::ByteArray)
			return false;

		peerAddress = QHostAddress(QString::fromUtf8(obj["peer-address"].toByteArray()));
	}

	peerPort = -1;
	if(obj.contains("peer-port"))
	{
		if(!canConvert(obj["peer-port"], VariantType::Int))
			return false;

		peerPort = obj["peer-port"].toInt();
	}

	connectHost.clear();
	if(obj.contains("connect-host"))
	{
		if(typeId(obj["connect-host"]) != VariantType::ByteArray)
			return false;

		connectHost = QString::fromUtf8(obj["connect-host"].toByteArray());
	}

	connectPort = -1;
	if(obj.contains("connect-port"))
	{
		if(!canConvert(obj["connect-port"], VariantType::Int))
			return false;

		connectPort = obj["connect-port"].toInt();
	}

	ignorePolicies = false;
	if(obj.contains("ignore-policies"))
	{
		if(typeId(obj["ignore-policies"]) != VariantType::Bool)
			return false;

		ignorePolicies = obj["ignore-policies"].toBool();
	}

	trustConnectHost = false;
	if(obj.contains("trust-connect-host"))
	{
		if(typeId(obj["trust-connect-host"]) != VariantType::Bool)
			return false;

		trustConnectHost = obj["trust-connect-host"].toBool();
	}

	ignoreTlsErrors = false;
	if(obj.contains("ignore-tls-errors"))
	{
		if(typeId(obj["ignore-tls-errors"]) != VariantType::Bool)
			return false;

		ignoreTlsErrors = obj["ignore-tls-errors"].toBool();
	}

	clientCert.clear();
	if(obj.contains("client-cert"))
	{
		if(typeId(obj["client-cert"]) != VariantType::ByteArray)
			return false;

		clientCert = QString::fromUtf8(obj["client-cert"].toByteArray());
	}

	clientKey.clear();
	if(obj.contains("client-key"))
	{
		if(typeId(obj["client-key"]) != VariantType::ByteArray)
			return false;

		clientKey = QString::fromUtf8(obj["client-key"].toByteArray());
	}

	followRedirects = false;
	if(obj.contains("follow-redirects"))
	{
		if(typeId(obj["follow-redirects"]) != VariantType::Bool)
			return false;

		followRedirects = obj["follow-redirects"].toBool();
	}

	passthrough = obj.value("passthrough");

	multi = false;
	if(obj.contains("ext"))
	{
		if(typeId(obj["ext"]) != VariantType::Hash)
			return false;

		VariantHash ext = obj["ext"].toHash();
		if(ext.contains("multi") && typeId(ext["multi"]) == VariantType::Bool)
		{
			multi = ext["multi"].toBool();
		}

		if(ext.contains("quiet") && typeId(ext["quiet"]) == VariantType::Bool)
		{
			quiet = ext["quiet"].toBool();
		}
	}

	return true;
}
