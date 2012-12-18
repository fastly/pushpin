/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
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

	QVariantList vrids;
	foreach(const Rid &r, rids)
	{
		QVariantHash vrid;
		vrid["sender"] = r.first;
		vrid["id"] = r.second;
		vrids += vrid;
	}

	obj["rids"] = vrids;

	{
		QVariantHash vrequest;

		vrequest["method"] = request.method.toLatin1();
		vrequest["path"] = request.path;

		QVariantList vheaders;
		foreach(const HttpHeader &h, request.headers)
		{
			QVariantList vheader;
			vheader += h.first;
			vheader += h.second;
			vheaders += QVariant(vheader);
		}

		vrequest["headers"] = vheaders;

		vrequest["body"] = request.body;

		obj["request"] = vrequest;
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
		vresponse["status"] = response.status;

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

        return obj;
}
