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

#include "acceptresponsepacket.h"

AcceptResponsePacket::AcceptResponsePacket() :
	haveInspectInfo(false),
	haveResponse(false)
{
}

QVariant AcceptResponsePacket::toVariant() const
{
	QVariantHash obj;

	{
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

			if(r.autoCrossOrigin)
				vrequest["auto-cross-origin"] = true;

			if(!r.jsonpCallback.isEmpty())
				vrequest["jsonp-callback"] = r.jsonpCallback;

			vrequest["in-seq"] = r.inSeq;
			vrequest["out-seq"] = r.outSeq;
			vrequest["out-credits"] = r.outCredits;
			if(r.userData.isValid())
				vrequest["user-data"] = r.userData;

			vrequests += vrequest;
		}

		obj["requests"] = vrequests;
	}

	{
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
	}

	if(haveInspectInfo)
	{
		QVariantHash vinspect;

		vinspect["no-proxy"] = inspectInfo.noProxy;
		vinspect["sharing-key"] = inspectInfo.sharingKey;

		if(inspectInfo.userData.isValid())
			vinspect["user-data"] = inspectInfo.userData;

		obj["inspect"] = vinspect;
	}

	if(haveResponse)
	{
		QVariantHash vresponse;

		vresponse["code"] = response.code;
		vresponse["reason"] = response.reason;

		QVariantList vheaders;
		foreach(const HttpHeader &h, response.headers)
		{
			QVariantList vheader;
			vheader += h.first;
			vheader += h.second;
			vheaders += QVariant(vheader);
		}
		vresponse["headers"] = vheaders;

		vresponse["body"] = response.body;

		obj["response"] = vresponse;
	}

	if(!channelPrefix.isEmpty())
		obj["channel-prefix"] = channelPrefix;

        return obj;
}
