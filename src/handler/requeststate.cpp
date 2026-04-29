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
#include "variant.h"

RequestState RequestState::fromVariant(const Variant &in)
{
	if(typeId(in) != VariantType::Hash)
		return RequestState();

	VariantHash r = in.toHash();
	RequestState rs;

	if(!r.contains("rid") || typeId(r["rid"]) != VariantType::Hash)
		return RequestState();

	VariantHash vrid = r["rid"].toHash();

	if(!vrid.contains("sender") || typeId(vrid["sender"]) != VariantType::ByteArray)
		return RequestState();

	if(!vrid.contains("id") || typeId(vrid["id"]) != VariantType::ByteArray)
		return RequestState();

	rs.rid = ZhttpRequest::Rid(vrid["sender"].toByteArray(), vrid["id"].toByteArray());

	if(!r.contains("in-seq") || !canConvert(r["in-seq"], VariantType::Int))
		return RequestState();

	rs.inSeq = r["in-seq"].toInt();

	if(!r.contains("out-seq") || !canConvert(r["out-seq"], VariantType::Int))
		return RequestState();

	rs.outSeq = r["out-seq"].toInt();

	if(!r.contains("out-credits") || !canConvert(r["out-credits"], VariantType::Int))
		return RequestState();

	rs.outCredits = r["out-credits"].toInt();

	if(r.contains("router-resp"))
	{
		if(typeId(r["router-resp"]) != VariantType::Bool)
			return RequestState();

		rs.routerResp = r["router-resp"].toBool();
	}

	if(r.contains("response-code"))
	{
		if(!canConvert(r["response-code"], VariantType::Int))
			return RequestState();

		rs.responseCode = r["response-code"].toInt();
	}

	if(r.contains("peer-address"))
	{
		if(typeId(r["peer-address"]) != VariantType::ByteArray)
			return RequestState();

		if(!rs.peerAddress.setAddress(QString::fromUtf8(r["peer-address"].toByteArray())))
			return RequestState();
	}

	if(r.contains("logical-peer-address"))
	{
		if(typeId(r["logical-peer-address"]) != VariantType::ByteArray)
			return RequestState();

		if(!rs.logicalPeerAddress.setAddress(QString::fromUtf8(r["logical-peer-address"].toByteArray())))
			return RequestState();
	}

	if(r.contains("https"))
	{
		if(typeId(r["https"]) != VariantType::Bool)
			return RequestState();

		rs.isHttps = r["https"].toBool();
	}

	if(r.contains("debug"))
	{
		if(typeId(r["debug"]) != VariantType::Bool)
			return RequestState();

		rs.debug = r["debug"].toBool();
	}

	if(r.contains("is-retry"))
	{
		if(typeId(r["is-retry"]) != VariantType::Bool)
			return RequestState();

		rs.isRetry = r["is-retry"].toBool();
	}

	if(r.contains("auto-cross-origin"))
	{
		if(typeId(r["auto-cross-origin"]) != VariantType::Bool)
			return RequestState();

		rs.autoCrossOrigin = r["auto-cross-origin"].toBool();
	}

	if(r.contains("jsonp-callback"))
	{
		if(typeId(r["jsonp-callback"]) != VariantType::ByteArray)
			return RequestState();

		rs.jsonpCallback = r["jsonp-callback"].toByteArray();
	}

	if(r.contains("jsonp-extended-response"))
	{
		if(typeId(r["jsonp-extended-response"]) != VariantType::Bool)
			return RequestState();

		rs.jsonpExtendedResponse = r["jsonp-extended-response"].toBool();
	}

	if(r.contains("unreported-time"))
	{
		if(!canConvert(r["unreported-time"], VariantType::Int))
			return RequestState();

		rs.unreportedTime = r["unreported-time"].toInt();
	}

	if(r.contains("user-data"))
	{
		rs.userData = r["user-data"];
	}

	return rs;
}
