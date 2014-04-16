/*
 * Copyright (C) 2014 Fanout, Inc.
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

#include "statspacket.h"

#include <assert.h>

QVariant StatsPacket::toVariant() const
{
	QVariantHash obj;

	obj["from"] = from;

	if(!route.isEmpty())
		obj["route"] = route;

	if(type == Activity)
	{
		obj["type"] = QByteArray("activity");

		int x = count;
		if(x < 0)
			x = 0;
		obj["count"] = x;
	}
	else // Connected/Disconnected
	{
		obj["id"] = connectionId;

		if(type == Connected)
		{
			if(connectionType == WebSocket)
				obj["type"] = QByteArray("ws");
			else // Http
				obj["type"] = QByteArray("http");

			obj["peer-address"] = peerAddress.toString().toUtf8();

			if(ssl)
				obj["ssl"] = true;

			obj["ttl"] = ttl;
		}
		else // Disconnected
		{
			obj["unavailable"] = true;
		}
	}

	return obj;
}
