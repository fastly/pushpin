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

bool ZurlResponsePacket::fromVariant(const QVariant &in)
{
	if(in.type() != QVariant::Hash)
		return false;

	QVariantHash obj = in.toHash();

	if(!obj.contains("id") || obj["id"].type() != QVariant::ByteArray)
		return false;
	id = obj["id"].toByteArray();

	seq = -1;
	if(obj.contains("seq"))
	{
		if(obj["seq"].type() != QVariant::Int)
			return false;

		seq = obj["seq"].toInt();
	}

	isError = false;
	if(obj.contains("error"))
	{
		if(obj["error"].type() != QVariant::Bool)
			return false;

		isError = obj["error"].toBool();
	}

	if(isError)
	{
		condition.clear();
		if(obj.contains("condition"))
		{
			if(obj["condition"].type() != QVariant::ByteArray)
				return false;

			condition = obj["condition"].toByteArray();
		}
	}
	else
	{
		replyAddress.clear();
		if(obj.contains("reply-address"))
		{
			if(obj["reply-address"].type() != QVariant::ByteArray)
				return false;

			replyAddress = obj["reply-address"].toByteArray();
		}

		code = -1;
		if(obj.contains("code"))
		{
			if(obj["code"].type() != QVariant::Int)
				return false;

			code = obj["code"].toInt();
		}

		status.clear();
		if(obj.contains("status"))
		{
			if(obj["status"].type() != QVariant::ByteArray)
				return false;

			status = obj["status"].toByteArray();
		}

		headers.clear();
		if(obj.contains("headers"))
		{
			if(obj["headers"].type() != QVariant::List)
				return false;

			foreach(const QVariant &i, obj["headers"].toList())
			{
				QVariantList list = i.toList();
				if(list.count() != 2)
					return false;

				if(list[0].type() != QVariant::ByteArray || list[1].type() != QVariant::ByteArray)
					return false;

				headers += QPair<QByteArray, QByteArray>(list[0].toByteArray(), list[1].toByteArray());
			}
		}

		body.clear();
		if(obj.contains("body"))
		{
			if(obj["body"].type() != QVariant::ByteArray)
				return false;

			body = obj["body"].toByteArray();
		}

		more = false;
		if(obj.contains("more"))
		{
			if(obj["more"].type() != QVariant::Bool)
				return false;

			more = obj["more"].toBool();
		}

		credits = -1;
		if(obj.contains("credits"))
		{
			if(obj["credits"].type() != QVariant::Int)
				return false;

			credits = obj["credits"].toInt();
		}
	}

	userData = obj["user-data"];

	return true;
}
