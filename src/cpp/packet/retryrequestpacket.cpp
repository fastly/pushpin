/*
 * Copyright (C) 2012-2023 Fanout, Inc.
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

#include "retryrequestpacket.h"

RetryRequestPacket::RetryRequestPacket() :
	haveInspectInfo(false),
	retrySeq(-1)
{
}

QVariant RetryRequestPacket::toVariant() const
{
	QVariantHash obj;

	QVariantList vrequests;
	foreach(const Request &r, requests)
	{
		QVariantHash vrequest;

		QVariantHash vrid;
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

		if(r.userData.isValid())
			vrequest["user-data"] = r.userData;

		vrequests += vrequest;
	}

	obj["requests"] = vrequests;

	QVariantHash vrequestData;

	vrequestData["method"] = requestData.method.toLatin1();
	vrequestData["uri"] = requestData.uri.toEncoded();

	QVariantList vheaders;
	foreach(const HttpHeader &h, requestData.headers)
	{
		QVariantList vheader;
		vheader += h.first;
		vheader += h.second;
		vheaders += QVariant(vheader);
	}
	vrequestData["headers"] = vheaders;

	vrequestData["body"] = requestData.body;

	obj["request-data"] = vrequestData;

	if(haveInspectInfo)
	{
		QVariantHash vinspect;

		vinspect["no-proxy"] = !inspectInfo.doProxy;

		if(!inspectInfo.sharingKey.isEmpty())
			vinspect["sharing-key"] = inspectInfo.sharingKey;

		if(!inspectInfo.sid.isEmpty())
			vinspect["sid"] = inspectInfo.sid;

		if(!inspectInfo.lastIds.isEmpty())
		{
			QVariantHash vlastIds;

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

bool RetryRequestPacket::fromVariant(const QVariant &in)
{
	if(in.type() != QVariant::Hash)
		return false;

	QVariantHash obj = in.toHash();

	if(!obj.contains("requests") || obj["requests"].type() != QVariant::List)
		return false;

	requests.clear();
	foreach(const QVariant &i, obj["requests"].toList())
	{
		if(i.type() != QVariant::Hash)
			return false;

		QVariantHash vrequest = i.toHash();

		Request r;

		if(!vrequest.contains("rid") || vrequest["rid"].type() != QVariant::Hash)
			return false;

		QVariantHash vrid = vrequest["rid"].toHash();

		QByteArray sender, id;

		if(!vrid.contains("sender") || vrid["sender"].type() != QVariant::ByteArray)
			return false;

		sender = vrid["sender"].toByteArray();

		if(!vrid.contains("id") || vrid["id"].type() != QVariant::ByteArray)
			return false;

		id = vrid["id"].toByteArray();

		r.rid = Rid(sender, id);

		if(vrequest.contains("https"))
		{
			if(vrequest["https"].type() != QVariant::Bool)
				return false;

			r.https = vrequest["https"].toBool();
		}

		if(vrequest.contains("peer-address"))
		{
			if(vrequest["peer-address"].type() != QVariant::ByteArray)
				return false;

			r.peerAddress = QHostAddress(QString::fromUtf8(vrequest["peer-address"].toByteArray()));
		}

		if(vrequest.contains("debug"))
		{
			if(vrequest["debug"].type() != QVariant::Bool)
				return false;

			r.debug = vrequest["debug"].toBool();
		}

		if(vrequest.contains("auto-cross-origin"))
		{
			if(vrequest["auto-cross-origin"].type() != QVariant::Bool)
				return false;

			r.autoCrossOrigin = vrequest["auto-cross-origin"].toBool();
		}

		if(vrequest.contains("jsonp-callback"))
		{
			if(vrequest["jsonp-callback"].type() != QVariant::ByteArray)
				return false;

			r.jsonpCallback = vrequest["jsonp-callback"].toByteArray();

			if(vrequest.contains("jsonp-extended-response"))
			{
				if(vrequest["jsonp-extended-response"].type() != QVariant::Bool)
					return false;

				r.jsonpExtendedResponse = vrequest["jsonp-extended-response"].toBool();
			}
		}

		if(vrequest.contains("unreported-time"))
		{
			if(!vrequest["unreported-time"].canConvert(QVariant::Int))
				return false;

			r.unreportedTime = vrequest["unreported-time"].toInt();
		}

		if(!vrequest.contains("in-seq") || !vrequest["in-seq"].canConvert(QVariant::Int))
			return false;
		r.inSeq = vrequest["in-seq"].toInt();

		if(!vrequest.contains("out-seq") || !vrequest["out-seq"].canConvert(QVariant::Int))
			return false;
		r.outSeq = vrequest["out-seq"].toInt();

		if(!vrequest.contains("out-credits") || !vrequest["out-credits"].canConvert(QVariant::Int))
			return false;
		r.outCredits = vrequest["out-credits"].toInt();

		if(vrequest.contains("user-data"))
			r.userData = vrequest["user-data"];

		requests += r;
	}

	if(!obj.contains("request-data") || obj["request-data"].type() != QVariant::Hash)
		return false;
	QVariantHash vrequestData = obj["request-data"].toHash();

	if(!vrequestData.contains("method") || vrequestData["method"].type() != QVariant::ByteArray)
		return false;
	requestData.method = QString::fromLatin1(vrequestData["method"].toByteArray());

	if(!vrequestData.contains("uri") || vrequestData["uri"].type() != QVariant::ByteArray)
		return false;
	requestData.uri = QUrl::fromEncoded(vrequestData["uri"].toByteArray(), QUrl::StrictMode);

	requestData.headers.clear();
	if(vrequestData.contains("headers"))
	{
		if(vrequestData["headers"].type() != QVariant::List)
			return false;

		foreach(const QVariant &i, vrequestData["headers"].toList())
		{
			QVariantList list = i.toList();
			if(list.count() != 2)
				return false;

			if(list[0].type() != QVariant::ByteArray || list[1].type() != QVariant::ByteArray)
				return false;

			requestData.headers += QPair<QByteArray, QByteArray>(list[0].toByteArray(), list[1].toByteArray());
		}
	}

	if(!vrequestData.contains("body") || vrequestData["body"].type() != QVariant::ByteArray)
		return false;
	requestData.body = vrequestData["body"].toByteArray();

	if(obj.contains("inspect"))
	{
		if(obj["inspect"].type() != QVariant::Hash)
			return false;
		QVariantHash vinspect = obj["inspect"].toHash();

		if(!vinspect.contains("no-proxy") || vinspect["no-proxy"].type() != QVariant::Bool)
			return false;
		inspectInfo.doProxy = !vinspect["no-proxy"].toBool();

		inspectInfo.sharingKey.clear();
		if(vinspect.contains("sharing-key"))
		{
			if(vinspect["sharing-key"].type() != QVariant::ByteArray)
				return false;

			inspectInfo.sharingKey = vinspect["sharing-key"].toByteArray();
		}

		if(vinspect.contains("sid"))
		{
			if(vinspect["sid"].type() != QVariant::ByteArray)
				return false;

			inspectInfo.sid = vinspect["sid"].toByteArray();
		}

		if(vinspect.contains("last-ids"))
		{
			if(vinspect["last-ids"].type() != QVariant::Hash)
				return false;

			QVariantHash vlastIds = vinspect["last-ids"].toHash();
			QHashIterator<QString, QVariant> it(vlastIds);
			while(it.hasNext())
			{
				it.next();

				if(it.value().type() != QVariant::ByteArray)
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
		if(obj["route"].type() != QVariant::ByteArray)
			return false;

		route = obj["route"].toByteArray();
	}

	if(obj.contains("retry-seq"))
	{
		if(!obj["retry-seq"].canConvert(QVariant::Int))
			return false;

		retrySeq = obj["retry-seq"].toInt();
	}

	return true;
}
