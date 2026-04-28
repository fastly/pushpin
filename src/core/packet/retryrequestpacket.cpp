/*
 * Copyright (C) 2012-2023 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
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

#include "retryrequestpacket.h"

#include "qtcompat.h"
#include "variant.h"

RetryRequestPacket::RetryRequestPacket() :
	haveInspectInfo(false),
	retrySeq(-1)
{
}

Variant RetryRequestPacket::toVariant() const
{
	VariantHash obj;

	VariantList vrequests;
	foreach(const Request &r, requests)
	{
		VariantHash vrequest;

		VariantHash vrid;
		vrid["sender"] = r.rid.first;
		vrid["id"] = r.rid.second;

		vrequest["rid"] = vrid;

		if(r.https)
			vrequest["https"] = true;

		if(!r.peerAddress.isNull())
			vrequest["peer-address"] = r.peerAddress.toString().toUtf8();

		if(r.debug)
			vrequest["debug"] = true;

		if(r.autoCrossOrigin)
			vrequest["auto-cross-origin"] = true;

		if(!r.jsonpCallback.isEmpty())
			vrequest["jsonp-callback"] = r.jsonpCallback;

		if(r.jsonpExtendedResponse)
			vrequest["jsonp-extended-response"] = true;

		if(r.unreportedTime > 0)
			vrequest["unreported-time"] = r.unreportedTime;

		vrequest["in-seq"] = r.inSeq;
		vrequest["out-seq"] = r.outSeq;
		vrequest["out-credits"] = r.outCredits;

		if(r.routerResp)
			vrequest["router-resp"] = r.routerResp;

		if(r.userData.isValid())
			vrequest["user-data"] = r.userData;

		vrequests += vrequest;
	}

	obj["requests"] = vrequests;

	VariantHash vrequestData;

	vrequestData["method"] = requestData.method.toLatin1();
	vrequestData["uri"] = requestData.uri.toEncoded();

	VariantList vheaders;
	foreach(const HttpHeader &h, requestData.headers)
	{
		VariantList vheader;
		vheader += h.first.asQByteArray();
		vheader += h.second.asQByteArray();
		vheaders += Variant(vheader);
	}
	vrequestData["headers"] = vheaders;

	vrequestData["body"] = requestData.body;

	obj["request-data"] = vrequestData;

	if(haveInspectInfo)
	{
		VariantHash vinspect;

		vinspect["no-proxy"] = !inspectInfo.doProxy;

		if(!inspectInfo.sharingKey.isEmpty())
			vinspect["sharing-key"] = inspectInfo.sharingKey;

		if(!inspectInfo.sid.isEmpty())
			vinspect["sid"] = inspectInfo.sid;

		if(!inspectInfo.lastIds.isEmpty())
		{
			VariantHash vlastIds;

			QHashIterator<QByteArray, QByteArray> it(inspectInfo.lastIds);
			while(it.hasNext())
			{
				it.next();

				vlastIds[QString::fromUtf8(it.key())] = it.value();
			}

			vinspect["last-ids"] = vlastIds;
		}

		if(inspectInfo.userData.isValid())
			vinspect["user-data"] = inspectInfo.userData;

		obj["inspect"] = vinspect;
	}

	if(!route.isEmpty())
		obj["route"] = route;

	if(retrySeq >= 0)
		obj["retry-seq"] = retrySeq;

	return obj;
}

bool RetryRequestPacket::fromVariant(const Variant &in)
{
	if(typeId(in) != VariantType::Hash)
		return false;

	VariantHash obj = in.toHash();

	if(!obj.contains("requests") || typeId(obj["requests"]) != VariantType::List)
		return false;

	requests.clear();
	for(const Variant &i : obj["requests"].toList())
	{
		if(typeId(i) != VariantType::Hash)
			return false;

		VariantHash vrequest = i.toHash();

		Request r;

		if(!vrequest.contains("rid") || typeId(vrequest["rid"]) != VariantType::Hash)
			return false;

		VariantHash vrid = vrequest["rid"].toHash();

		QByteArray sender, id;

		if(!vrid.contains("sender") || typeId(vrid["sender"]) != VariantType::ByteArray)
			return false;

		sender = vrid["sender"].toByteArray();

		if(!vrid.contains("id") || typeId(vrid["id"]) != VariantType::ByteArray)
			return false;

		id = vrid["id"].toByteArray();

		r.rid = Rid(sender, id);

		if(vrequest.contains("https"))
		{
			if(typeId(vrequest["https"]) != VariantType::Bool)
				return false;

			r.https = vrequest["https"].toBool();
		}

		if(vrequest.contains("peer-address"))
		{
			if(typeId(vrequest["peer-address"]) != VariantType::ByteArray)
				return false;

			r.peerAddress = QHostAddress(QString::fromUtf8(vrequest["peer-address"].toByteArray()));
		}

		if(vrequest.contains("debug"))
		{
			if(typeId(vrequest["debug"]) != VariantType::Bool)
				return false;

			r.debug = vrequest["debug"].toBool();
		}

		if(vrequest.contains("auto-cross-origin"))
		{
			if(typeId(vrequest["auto-cross-origin"]) != VariantType::Bool)
				return false;

			r.autoCrossOrigin = vrequest["auto-cross-origin"].toBool();
		}

		if(vrequest.contains("jsonp-callback"))
		{
			if(typeId(vrequest["jsonp-callback"]) != VariantType::ByteArray)
				return false;

			r.jsonpCallback = vrequest["jsonp-callback"].toByteArray();

			if(vrequest.contains("jsonp-extended-response"))
			{
				if(typeId(vrequest["jsonp-extended-response"]) != VariantType::Bool)
					return false;

				r.jsonpExtendedResponse = vrequest["jsonp-extended-response"].toBool();
			}
		}

		if(vrequest.contains("unreported-time"))
		{
			if(!canConvert(vrequest["unreported-time"], VariantType::Int))
				return false;

			r.unreportedTime = vrequest["unreported-time"].toInt();
		}

		if(!vrequest.contains("in-seq") || !canConvert(vrequest["in-seq"], VariantType::Int))
			return false;
		r.inSeq = vrequest["in-seq"].toInt();

		if(!vrequest.contains("out-seq") || !canConvert(vrequest["out-seq"], VariantType::Int))
			return false;
		r.outSeq = vrequest["out-seq"].toInt();

		if(!vrequest.contains("out-credits") || !canConvert(vrequest["out-credits"], VariantType::Int))
			return false;
		r.outCredits = vrequest["out-credits"].toInt();

		if(vrequest.contains("router-resp"))
		{
			if(typeId(vrequest["router-resp"]) != VariantType::Bool)
				return false;

			r.routerResp = vrequest["router-resp"].toBool();
		}

		if(vrequest.contains("user-data"))
			r.userData = vrequest["user-data"];

		requests += r;
	}

	if(!obj.contains("request-data") || typeId(obj["request-data"]) != VariantType::Hash)
		return false;
	VariantHash vrequestData = obj["request-data"].toHash();

	if(!vrequestData.contains("method") || typeId(vrequestData["method"]) != VariantType::ByteArray)
		return false;
	requestData.method = QString::fromLatin1(vrequestData["method"].toByteArray());

	if(!vrequestData.contains("uri") || typeId(vrequestData["uri"]) != VariantType::ByteArray)
		return false;
	requestData.uri = Url::fromEncoded(vrequestData["uri"].toByteArray(), Url::StrictMode);

	requestData.headers.clear();
	if(vrequestData.contains("headers"))
	{
		if(typeId(vrequestData["headers"]) != VariantType::List)
			return false;

		for(const Variant &i : vrequestData["headers"].toList())
		{
			VariantList list = i.toList();
			if(list.count() != 2)
				return false;

			if(typeId(list[0]) != VariantType::ByteArray || typeId(list[1]) != VariantType::ByteArray)
				return false;

			requestData.headers += QPair<QByteArray, QByteArray>(list[0].toByteArray(), list[1].toByteArray());
		}
	}

	if(!vrequestData.contains("body") || typeId(vrequestData["body"]) != VariantType::ByteArray)
		return false;
	requestData.body = vrequestData["body"].toByteArray();

	if(obj.contains("inspect"))
	{
		if(typeId(obj["inspect"]) != VariantType::Hash)
			return false;
		VariantHash vinspect = obj["inspect"].toHash();

		if(!vinspect.contains("no-proxy") || typeId(vinspect["no-proxy"]) != VariantType::Bool)
			return false;
		inspectInfo.doProxy = !vinspect["no-proxy"].toBool();

		inspectInfo.sharingKey.clear();
		if(vinspect.contains("sharing-key"))
		{
			if(typeId(vinspect["sharing-key"]) != VariantType::ByteArray)
				return false;

			inspectInfo.sharingKey = vinspect["sharing-key"].toByteArray();
		}

		if(vinspect.contains("sid"))
		{
			if(typeId(vinspect["sid"]) != VariantType::ByteArray)
				return false;

			inspectInfo.sid = vinspect["sid"].toByteArray();
		}

		if(vinspect.contains("last-ids"))
		{
			if(typeId(vinspect["last-ids"]) != VariantType::Hash)
				return false;

			VariantHash vlastIds = vinspect["last-ids"].toHash();
			for(auto it = vlastIds.constBegin(); it != vlastIds.constEnd(); ++it)
			{
				if(typeId(it.value()) != VariantType::ByteArray)
					return false;

				QByteArray key = it.key().toUtf8();
				QByteArray val = it.value().toByteArray();
				inspectInfo.lastIds.insert(key, val);
			}
		}

		inspectInfo.userData = vinspect["user-data"];

		haveInspectInfo = true;
	}

	if(obj.contains("route"))
	{
		if(typeId(obj["route"]) != VariantType::ByteArray)
			return false;

		route = obj["route"].toByteArray();
	}

	if(obj.contains("retry-seq"))
	{
		if(!canConvert(obj["retry-seq"], VariantType::Int))
			return false;

		retrySeq = obj["retry-seq"].toInt();
	}

	return true;
}
