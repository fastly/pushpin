/*
 * Copyright (C) 2012-2015 Fanout, Inc.
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

#include "inspectrequest.h"

#include "packet/httprequestdata.h"
#include "qtcompat.h"
#include "inspectdata.h"

static InspectData resultToData(const QVariant &in, bool *ok)
{
	InspectData out;

	if(typeId(in) != QMetaType::QVariantHash)
	{
		*ok = false;
		return InspectData();
	}

	QVariantHash obj = in.toHash();

	if(!obj.contains("no-proxy") || typeId(obj["no-proxy"]) != QMetaType::Bool)
	{
		*ok = false;
		return InspectData();
	}
	out.doProxy = !obj["no-proxy"].toBool();

	out.sharingKey.clear();
	if(obj.contains("sharing-key"))
	{
		if(typeId(obj["sharing-key"]) != QMetaType::QByteArray)
		{
			*ok = false;
			return InspectData();
		}

		out.sharingKey = obj["sharing-key"].toByteArray();
	}

	out.sid.clear();
	if(obj.contains("sid"))
	{
		if(typeId(obj["sid"]) != QMetaType::QByteArray)
		{
			*ok = false;
			return InspectData();
		}

		out.sid = obj["sid"].toByteArray();
	}

	out.lastIds.clear();
	if(obj.contains("last-ids"))
	{
		if(typeId(obj["last-ids"]) != QMetaType::QVariantHash)
		{
			*ok = false;
			return InspectData();
		}

		QVariantHash vlastIds = obj["last-ids"].toHash();
		QHashIterator<QString, QVariant> it(vlastIds);
		while(it.hasNext())
		{
			it.next();

			if(typeId(it.value()) != QMetaType::QByteArray)
			{
				*ok = false;
				return InspectData();
			}

			QByteArray key = it.key().toUtf8();
			QByteArray val = it.value().toByteArray();
			out.lastIds.insert(key, val);
		}
	}

	out.userData = obj["user-data"];

	*ok = true;
	return out;
}

class InspectRequest::Private
{
public:
	InspectRequest *q;
	InspectData idata;

	Private(InspectRequest *_q) :
		q(_q)
	{
	}
};

InspectRequest::InspectRequest(ZrpcManager *manager) :
	ZrpcRequest(manager)
{
	d = std::make_unique<Private>(this);
}

InspectRequest::~InspectRequest() = default;

InspectData InspectRequest::result() const
{
	return d->idata;
}

void InspectRequest::start(const HttpRequestData &hdata, bool truncated, bool getSession, bool autoShare)
{
	QVariantHash args;

	args["method"] = hdata.method.toLatin1();
	args["uri"] = hdata.uri.toEncoded();

	QVariantList vheaders;
	foreach(const HttpHeader &h, hdata.headers)
	{
		QVariantList vheader;
		vheader += h.first;
		vheader += h.second;
		vheaders += QVariant(vheader);
	}

	args["headers"] = vheaders;
	args["body"] = hdata.body;

	if(truncated)
		args["truncated"] = true;

	if(getSession)
		args["get-session"] = true;

	if(autoShare)
		args["auto-share"] = true;

	ZrpcRequest::start("inspect", args);
}

void InspectRequest::onSuccess()
{
	bool ok;
	d->idata = resultToData(ZrpcRequest::result(), &ok);
	if(!ok)
	{
		setError(ErrorFormat);
		return;
	}
}

