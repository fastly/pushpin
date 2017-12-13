/*
 * Copyright (C) 2012-2015 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
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
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
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
