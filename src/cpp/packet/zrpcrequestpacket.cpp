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

#include "zrpcrequestpacket.h"

#include "qtcompat.h"

QVariant ZrpcRequestPacket::toVariant() const
{
	QVariantHash obj;

	if(!from.isEmpty())
		obj["from"] = from;

	if(!id.isEmpty())
		obj["id"] = id;

	obj["method"] = method.toUtf8();

	if(!args.isEmpty())
		obj["args"] = args;

	return obj;
}

bool ZrpcRequestPacket::fromVariant(const QVariant &in)
{
	if(typeId(in) != QMetaType::QVariantHash)
		return false;

	QVariantHash obj = in.toHash();

	if(obj.contains("from"))
	{
		if(typeId(obj["from"]) != QMetaType::QByteArray)
			return false;

		from = obj["from"].toByteArray();
	}

	if(obj.contains("id"))
	{
		if(typeId(obj["id"]) != QMetaType::QByteArray)
			return false;

		id = obj["id"].toByteArray();
	}

	if(!obj.contains("method") || typeId(obj["method"]) != QMetaType::QByteArray)
		return false;
	method = QString::fromUtf8(obj["method"].toByteArray());

	if(obj.contains("args"))
	{
		if(typeId(obj["args"]) != QMetaType::QVariantHash)
			return false;

		args = obj["args"].toHash();
	}

	return true;
}
