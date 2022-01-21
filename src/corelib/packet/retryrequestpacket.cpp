/*
 * Copyright (C) 2012-2022 Fanout, Inc.
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

#include "retryrequestpacket.h"

RetryRequestPacket::RetryRequestPacket() :
	haveInspectInfo(false)
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

	return true;
}
