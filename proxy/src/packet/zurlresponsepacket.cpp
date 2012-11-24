/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "zurlresponsepacket.h"

ZurlResponsePacket::ZurlResponsePacket() :
	seq(-1),
	isError(false),
	code(-1),
	more(false),
	credits(-1)
{
}

QVariant ZurlResponsePacket::toVariant() const
{
	QVariantHash obj;
	obj["id"] = id;

	if(isError)
	{
		obj["error"] = true;
		obj["condition"] = condition;
	}
	else
	{
		if(seq != -1)
			obj["seq"] = seq;

		if(!replyAddress.isEmpty())
			obj["reply-address"] = replyAddress;

		if(code != -1)
		{
			obj["code"] = code;
			obj["status"] = status;
			QVariantList vheaders;
			foreach(const HttpHeader &h, headers)
			{
				QVariantList vheader;
				vheader += h.first;
				vheader += h.second;
				vheaders += QVariant(vheader);
			}
			obj["headers"] = vheaders;
		}

		if(!body.isNull())
			obj["body"] = body;

		if(more)
			obj["more"] = true;

		if(credits != -1)
			obj["credits"] = credits;
	}

	if(userData.isValid())
		obj["user-data"] = userData;

	return obj;
}
