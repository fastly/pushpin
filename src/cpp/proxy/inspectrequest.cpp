/*
 * Copyright (C) 2012-2015 Fanout, Inc.
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
#include "inspectdata.h"

static InspectData resultToData(const QVariant &in, bool *ok)
{
	InspectData out;

	if(in.type() != QVariant::Hash)
	{
		*ok = false;
		return InspectData();
	}

	QVariantHash obj = in.toHash();

	if(!obj.contains("no-proxy") || obj["no-proxy"].type() != QVariant::Bool)
	{
		*ok = false;
		return InspectData();
	}
	out.doProxy = !obj["no-proxy"].toBool();

	out.sharingKey.clear();
	if(obj.contains("sharing-key"))
	{
		if(obj["sharing-key"].type() != QVariant::ByteArray)
		{
			*ok = false;
			return InspectData();
		}

		out.sharingKey = obj["sharing-key"].toByteArray();
	}

	out.sid.clear();
	if(obj.contains("sid"))
	{
		if(obj["sid"].type() != QVariant::ByteArray)
		{
			*ok = false;
			return InspectData();
		}

		out.sid = obj["sid"].toByteArray();
	}

	out.lastIds.clear();
	if(obj.contains("last-ids"))
	{
		if(obj["last-ids"].type() != QVariant::Hash)
		{
			*ok = false;
			return InspectData();
		}

		QVariantHash vlastIds = obj["last-ids"].toHash();
		QHashIterator<QString, QVariant> it(vlastIds);
		while(it.hasNext())
		{
			it.next();

			if(it.value().type() != QVariant::ByteArray)
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

class InspectRequest::Private : public QObject
{
	Q_OBJECT

public:
	InspectRequest *q;
	InspectData idata;

	Private(InspectRequest *_q) :
		QObject(_q),
		q(_q)
	{
	}
};

InspectRequest::InspectRequest(ZrpcManager *manager, QObject *parent) :
	ZrpcRequest(manager, parent)
{
	d = new Private(this);
}

InspectRequest::~InspectRequest()
{
	delete d;
}

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

#include "inspectrequest.moc"
