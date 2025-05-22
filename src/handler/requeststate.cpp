/*
 * Copyright (C) 2016-2023 Fanout, Inc.
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

#include "requeststate.h"

#include "qtcompat.h"

RequestState RequestState::fromVariant(const QVariant &in)
{
	if(typeId(in) != QMetaType::QVariantHash)
		return RequestState();

	QVariantHash r = in.toHash();
	RequestState rs;

	if(!r.contains("rid") || typeId(r["rid"]) != QMetaType::QVariantHash)
		return RequestState();

	QVariantHash vrid = r["rid"].toHash();

	if(!vrid.contains("sender") || typeId(vrid["sender"]) != QMetaType::QByteArray)
		return RequestState();

	if(!vrid.contains("id") || typeId(vrid["id"]) != QMetaType::QByteArray)
		return RequestState();

	rs.rid = ZhttpRequest::Rid(vrid["sender"].toByteArray(), vrid["id"].toByteArray());

	if(!r.contains("in-seq") || !canConvert(r["in-seq"], QMetaType::Int))
		return RequestState();

	rs.inSeq = r["in-seq"].toInt();

	if(!r.contains("out-seq") || !canConvert(r["out-seq"], QMetaType::Int))
		return RequestState();

	rs.outSeq = r["out-seq"].toInt();

	if(!r.contains("out-credits") || !canConvert(r["out-credits"], QMetaType::Int))
		return RequestState();

	rs.outCredits = r["out-credits"].toInt();

	if(r.contains("router-resp"))
	{
		if(typeId(r["router-resp"]) != QMetaType::Bool)
			return RequestState();

		rs.routerResp = r["router-resp"].toBool();
	}

	if(r.contains("response-code"))
	{
		if(!canConvert(r["response-code"], QMetaType::Int))
			return RequestState();

		rs.responseCode = r["response-code"].toInt();
	}

	if(r.contains("peer-address"))
	{
		if(typeId(r["peer-address"]) != QMetaType::QByteArray)
			return RequestState();

		if(!rs.peerAddress.setAddress(QString::fromUtf8(r["peer-address"].toByteArray())))
			return RequestState();
	}

	if(r.contains("logical-peer-address"))
	{
		if(typeId(r["logical-peer-address"]) != QMetaType::QByteArray)
			return RequestState();

		if(!rs.logicalPeerAddress.setAddress(QString::fromUtf8(r["logical-peer-address"].toByteArray())))
			return RequestState();
	}

	if(r.contains("https"))
	{
		if(typeId(r["https"]) != QMetaType::Bool)
			return RequestState();

		rs.isHttps = r["https"].toBool();
	}

	if(r.contains("debug"))
	{
		if(typeId(r["debug"]) != QMetaType::Bool)
			return RequestState();

		rs.debug = r["debug"].toBool();
	}

	if(r.contains("is-retry"))
	{
		if(typeId(r["is-retry"]) != QMetaType::Bool)
			return RequestState();

		rs.isRetry = r["is-retry"].toBool();
	}

	if(r.contains("auto-cross-origin"))
	{
		if(typeId(r["auto-cross-origin"]) != QMetaType::Bool)
			return RequestState();

		rs.autoCrossOrigin = r["auto-cross-origin"].toBool();
	}

	if(r.contains("jsonp-callback"))
	{
		if(typeId(r["jsonp-callback"]) != QMetaType::QByteArray)
			return RequestState();

		rs.jsonpCallback = r["jsonp-callback"].toByteArray();
	}

	if(r.contains("jsonp-extended-response"))
	{
		if(typeId(r["jsonp-extended-response"]) != QMetaType::Bool)
			return RequestState();

		rs.jsonpExtendedResponse = r["jsonp-extended-response"].toBool();
	}

	if(r.contains("unreported-time"))
	{
		if(!canConvert(r["unreported-time"], QMetaType::Int))
			return RequestState();

		rs.unreportedTime = r["unreported-time"].toInt();
	}

	if(r.contains("user-data"))
	{
		rs.userData = r["user-data"];
	}

	return rs;
}
