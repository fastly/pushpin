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

#include "inspectresponsepacket.h"

InspectResponsePacket::InspectResponsePacket() :
	noProxy(false)
{
}

bool InspectResponsePacket::fromVariant(const QVariant &in)
{
	if(in.type() != QVariant::Hash)
		return false;

	QVariantHash obj = in.toHash();

	if(!obj.contains("id") || obj["id"].type() != QVariant::ByteArray)
		return false;
	id = obj["id"].toByteArray();

	if(!obj.contains("no-proxy") || obj["no-proxy"].type() != QVariant::Bool)
		return false;
	noProxy = obj["no-proxy"].toBool();

	sharingKey.clear();
	if(obj.contains("sharing-key"))
	{
		if(obj["sharing-key"].type() != QVariant::ByteArray)
			return false;

		sharingKey = obj["sharing-key"].toByteArray();
	}

	userData = obj["user-data"];

	return true;
}
