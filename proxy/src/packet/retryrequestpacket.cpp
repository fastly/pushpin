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

	if(!obj.contains("rids") || obj["rids"].type() != QVariant::List)
		return false;

	rids.clear();
	foreach(const QVariant &i, obj["rids"].toList())
	{
		if(i.type() != QVariant::Hash)
			return false;

		QVariantHash vrid = i.toHash();
		QByteArray sender, id;

		if(!vrid.contains("sender") || vrid["sender"].type() != QVariant::ByteArray)
			return false;
		sender = vrid["sender"].toByteArray();

		if(!vrid.contains("id") || vrid["id"].type() != QVariant::ByteArray)
			return false;
		id = vrid["id"].toByteArray();

		rids += Rid(sender, id);
	}

	if(!obj.contains("request") || obj["request"].type() != QVariant::Hash)
		return false;
	QVariantHash vrequest = obj["request"].toHash();

	if(!vrequest.contains("method") || vrequest["method"].type() != QVariant::ByteArray)
		return false;
	request.method = QString::fromLatin1(vrequest["method"].toByteArray());

	if(!vrequest.contains("path") || vrequest["path"].type() != QVariant::ByteArray)
		return false;
	request.path = vrequest["path"].toByteArray();

	request.headers.clear();
	if(vrequest.contains("headers"))
	{
		if(vrequest["headers"].type() != QVariant::List)
			return false;

		foreach(const QVariant &i, vrequest["headers"].toList())
		{
			QVariantList list = i.toList();
			if(list.count() != 2)
				return false;

			if(list[0].type() != QVariant::ByteArray || list[1].type() != QVariant::ByteArray)
				return false;

			request.headers += QPair<QByteArray, QByteArray>(list[0].toByteArray(), list[1].toByteArray());
		}
	}

	if(!vrequest.contains("body") || vrequest["body"].type() != QVariant::ByteArray)
		return false;
	request.body = vrequest["body"].toByteArray();

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
