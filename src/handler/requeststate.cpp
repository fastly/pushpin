/*
 * Copyright (C) 2016-2023 Fanout, Inc.
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

RequestState RequestState::fromVariant(const QVariant &in)
{
	if(in.type() != QVariant::Hash)
		return RequestState();

	QVariantHash r = in.toHash();
	RequestState rs;

	if(!r.contains("rid") || r["rid"].type() != QVariant::Hash)
		return RequestState();

	QVariantHash vrid = r["rid"].toHash();

	if(!vrid.contains("sender") || vrid["sender"].type() != QVariant::ByteArray)
		return RequestState();

	if(!vrid.contains("id") || vrid["id"].type() != QVariant::ByteArray)
		return RequestState();

	rs.rid = ZhttpRequest::Rid(vrid["sender"].toByteArray(), vrid["id"].toByteArray());

	if(!r.contains("in-seq") || !r["in-seq"].canConvert(QVariant::Int))
		return RequestState();

	rs.inSeq = r["in-seq"].toInt();

	if(!r.contains("out-seq") || !r["out-seq"].canConvert(QVariant::Int))
		return RequestState();

	rs.outSeq = r["out-seq"].toInt();

	if(!r.contains("out-credits") || !r["out-credits"].canConvert(QVariant::Int))
		return RequestState();

	rs.outCredits = r["out-credits"].toInt();

	if(r.contains("response-code"))
	{
		if(!r["response-code"].canConvert(QVariant::Int))
			return RequestState();

		rs.responseCode = r["response-code"].toInt();
	}

	if(r.contains("peer-address"))
	{
		if(r["peer-address"].type() != QVariant::ByteArray)
			return RequestState();

		if(!rs.peerAddress.setAddress(QString::fromUtf8(r["peer-address"].toByteArray())))
			return RequestState();
	}

	if(r.contains("logical-peer-address"))
	{
		if(r["logical-peer-address"].type() != QVariant::ByteArray)
			return RequestState();

		if(!rs.logicalPeerAddress.setAddress(QString::fromUtf8(r["logical-peer-address"].toByteArray())))
			return RequestState();
	}

	if(r.contains("https"))
	{
		if(r["https"].type() != QVariant::Bool)
			return RequestState();

		rs.isHttps = r["https"].toBool();
	}

	if(r.contains("debug"))
	{
		if(r["debug"].type() != QVariant::Bool)
			return RequestState();

		rs.debug = r["debug"].toBool();
	}

	if(r.contains("is-retry"))
	{
		if(r["is-retry"].type() != QVariant::Bool)
			return RequestState();

		rs.isRetry = r["is-retry"].toBool();
	}

	if(r.contains("auto-cross-origin"))
	{
		if(r["auto-cross-origin"].type() != QVariant::Bool)
			return RequestState();

		rs.autoCrossOrigin = r["auto-cross-origin"].toBool();
	}

	if(r.contains("jsonp-callback"))
	{
		if(r["jsonp-callback"].type() != QVariant::ByteArray)
			return RequestState();

		rs.jsonpCallback = r["jsonp-callback"].toByteArray();
	}

	if(r.contains("jsonp-extended-response"))
	{
		if(r["jsonp-extended-response"].type() != QVariant::Bool)
			return RequestState();

		rs.jsonpExtendedResponse = r["jsonp-extended-response"].toBool();
	}

	if(r.contains("unreported-time"))
	{
		if(!r["unreported-time"].canConvert(QVariant::Int))
			return RequestState();

		rs.unreportedTime = r["unreported-time"].toInt();
	}

	if(r.contains("user-data"))
	{
		rs.userData = r["user-data"];
	}

	return rs;
}
