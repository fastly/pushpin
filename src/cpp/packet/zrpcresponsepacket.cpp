/*
 * Copyright (C) 2014 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include "zrpcresponsepacket.h"

#include "qtcompat.h"

QVariant ZrpcResponsePacket::toVariant() const
{
	QVariantHash obj;

	if(!id.isEmpty())
		obj["id"] = id;

	obj["success"] = success;

	if(success)
	{
		if(typeId(value) == QMetaType::QString)
			obj["value"] = value.toString().toUtf8();
		else
			obj["value"] = value;
	}
	else
	{
		obj["condition"] = condition;

		if(value.isValid())
		{
			if(typeId(value) == QMetaType::QString)
				obj["value"] = value.toString().toUtf8();
			else
				obj["value"] = value;
		}
	}

	return obj;
}

bool ZrpcResponsePacket::fromVariant(const QVariant &in)
{
	if(typeId(in) != QMetaType::QVariantHash)
		return false;

	QVariantHash obj = in.toHash();

	if(obj.contains("id"))
	{
		if(typeId(obj["id"]) != QMetaType::QByteArray)
			return false;

		id = obj["id"].toByteArray();
	}

	if(!obj.contains("success") || typeId(obj["success"]) != QMetaType::Bool)
		return false;
	success = obj["success"].toBool();

	value.clear();
	condition.clear();
	if(success)
	{
		if(!obj.contains("value"))
			return false;
		value = obj["value"];
	}
	else
	{
		if(!obj.contains("condition") || typeId(obj["condition"]) != QMetaType::QByteArray)
			return false;
		condition = obj["condition"].toByteArray();

		if(obj.contains("value"))
			value = obj["value"];
	}

	return true;
}
