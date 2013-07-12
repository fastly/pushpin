/*
 * Copyright (C) 2012-2013 Fanout, Inc.
 *
 * This file is part of Pushpin.
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
 */

#include "retryrequestpacket.h"

RetryRequestPacket::RetryRequestPacket() :
	haveInspectInfo(false)
{
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
		}

		if(!vrequest.contains("in-seq") || vrequest["in-seq"].type() != QVariant::Int)
			return false;
		r.inSeq = vrequest["in-seq"].toInt();

		if(!vrequest.contains("out-seq") || vrequest["out-seq"].type() != QVariant::Int)
			return false;
		r.outSeq = vrequest["out-seq"].toInt();

		if(!vrequest.contains("out-credits") || vrequest["out-credits"].type() != QVariant::Int)
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
		inspectInfo.noProxy = vinspect["no-proxy"].toBool();

		inspectInfo.sharingKey.clear();
		if(vinspect.contains("sharing-key"))
		{
			if(vinspect["sharing-key"].type() != QVariant::ByteArray)
				return false;

			inspectInfo.sharingKey = vinspect["sharing-key"].toByteArray();
		}

		inspectInfo.userData = vinspect["user-data"];

		haveInspectInfo = true;
	}

	return true;
}
