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

#include "inspectrequestpacket.h"

#include "tnetstring.h"

InspectRequestPacket::InspectRequestPacket()
{
}

QVariant InspectRequestPacket::toVariant() const
{
	QVariantHash obj;
	obj["id"] = id;
	obj["method"] = method.toLatin1();
	obj["path"] = path;

	QVariantList vheaders;
	foreach(const HttpHeader &h, headers)
	{
		QVariantList vheader;
		vheader += h.first;
		vheader += h.second;
		vheaders += QVariant(vheader);
	}

	obj["headers"] = vheaders;

	return obj;
}
