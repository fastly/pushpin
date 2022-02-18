/*
 * Copyright (C) 2015-2022 Fanout, Inc.
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

#include "acceptrequest.h"

#include "acceptdata.h"

static QVariant acceptDataToVariant(const AcceptData &adata)
{
	QVariantHash obj;

	{
		QVariantList vrequests;
		foreach(const AcceptData::Request &r, adata.requests)
		{
			QVariantHash vrequest;

			QVariantHash vrid;
			vrid["sender"] = r.rid.first;
			vrid["id"] = r.rid.second;

			vrequest["rid"] = vrid;

			if(r.https)
				vrequest["https"] = true;

			if(!r.peerAddress.isNull())
				vrequest["peer-address"] = r.peerAddress.toString().toUtf8();

			if(!r.logicalPeerAddress.isNull())
				vrequest["logical-peer-address"] = r.logicalPeerAddress.toString().toUtf8();

			if(r.debug)
				vrequest["debug"] = true;

			if(r.isRetry)
				vrequest["is-retry"] = true;

			if(r.autoCrossOrigin)
				vrequest["auto-cross-origin"] = true;

			if(!r.jsonpCallback.isEmpty())
			{
				vrequest["jsonp-callback"] = r.jsonpCallback;

				if(r.jsonpExtendedResponse)
					vrequest["jsonp-extended-response"] = true;
			}

			if(r.responseCode != -1)
				vrequest["response-code"] = r.responseCode;

			vrequest["in-seq"] = r.inSeq;
			vrequest["out-seq"] = r.outSeq;
			vrequest["out-credits"] = r.outCredits;
			if(r.userData.isValid())
				vrequest["user-data"] = r.userData;

			vrequests += vrequest;
		}

		obj["requests"] = vrequests;
	}

	{
		const HttpRequestData &requestData = adata.requestData;
		QVariantHash vrequestData;

		vrequestData["method"] = requestData.method.toLatin1();
		vrequestData["uri"] = requestData.uri.toEncoded();

		QVariantList vheaders;
		foreach(const HttpHeader &h, requestData.headers)
		{
			QVariantList vheader;
			vheader += h.first;
			vheader += h.second;
			vheaders += QVariant(vheader);
		}

		vrequestData["headers"] = vheaders;

		vrequestData["body"] = requestData.body;

		obj["request-data"] = vrequestData;
	}

	{
		const HttpRequestData &requestData = adata.origRequestData;
		QVariantHash vrequestData;

		vrequestData["method"] = requestData.method.toLatin1();
		vrequestData["uri"] = requestData.uri.toEncoded();

		QVariantList vheaders;
		foreach(const HttpHeader &h, requestData.headers)
		{
			QVariantList vheader;
			vheader += h.first;
			vheader += h.second;
			vheaders += QVariant(vheader);
		}

		vrequestData["headers"] = vheaders;

		vrequestData["body"] = requestData.body;

		obj["orig-request-data"] = vrequestData;
	}

	if(adata.haveInspectData)
	{
		QVariantHash vinspect;

		vinspect["no-proxy"] = !adata.inspectData.doProxy;

		if(!adata.inspectData.sharingKey.isEmpty())
			vinspect["sharing-key"] = adata.inspectData.sharingKey;

		if(!adata.inspectData.sid.isEmpty())
			vinspect["sid"] = adata.inspectData.sid;

		if(!adata.inspectData.lastIds.isEmpty())
		{
			QVariantHash vlastIds;
			QHashIterator<QByteArray, QByteArray> it(adata.inspectData.lastIds);
			while(it.hasNext())
			{
				it.next();
				vlastIds[QString::fromUtf8(it.key())] = it.value();
			}

			vinspect["last-ids"] = vlastIds;
		}

		if(adata.inspectData.userData.isValid())
			vinspect["user-data"] = adata.inspectData.userData;

		obj["inspect"] = vinspect;
	}

	if(adata.haveResponse)
	{
		QVariantHash vresponse;

		vresponse["code"] = adata.response.code;
		vresponse["reason"] = adata.response.reason;

		QVariantList vheaders;
		foreach(const HttpHeader &h, adata.response.headers)
		{
			QVariantList vheader;
			vheader += h.first;
			vheader += h.second;
			vheaders += QVariant(vheader);
		}
		vresponse["headers"] = vheaders;

		vresponse["body"] = adata.response.body;

		obj["response"] = vresponse;
	}

	if(!adata.route.isEmpty())
		obj["route"] = adata.route;

	if(adata.separateStats)
		obj["separate-stats"] = true;

	if(!adata.channelPrefix.isEmpty())
		obj["channel-prefix"] = adata.channelPrefix;

	if(!adata.channels.isEmpty())
	{
		QVariantList vchannels;
		foreach(const QByteArray &channel, adata.channels)
			vchannels += channel;
		obj["channels"] = vchannels;
	}

	if(!adata.sigIss.isEmpty())
		obj["sig-iss"] = adata.sigIss;

	if(!adata.sigKey.isEmpty())
		obj["sig-key"] = adata.sigKey;

	if(adata.trusted)
		obj["trusted"] = true;

	if(adata.useSession)
		obj["use-session"] = true;

	if(adata.responseSent)
		obj["response-sent"] = true;

	return obj;
}

static AcceptRequest::ResponseData convertResult(const QVariant &in, bool *ok)
{
	AcceptRequest::ResponseData out;

	if(in.type() != QVariant::Hash)
	{
		*ok = false;
		return AcceptRequest::ResponseData();
	}

	QVariantHash obj = in.toHash();

	if(obj.contains("accepted"))
	{
		if(obj["accepted"].type() != QVariant::Bool)
		{
			*ok = false;
			return AcceptRequest::ResponseData();
		}

		out.accepted = obj["accepted"].toBool();
	}

	if(obj.contains("response"))
	{
		if(obj["response"].type() != QVariant::Hash)
		{
			*ok = false;
			return AcceptRequest::ResponseData();
		}

		QVariantHash vresponse = obj["response"].toHash();

		if(vresponse.contains("code"))
		{
			if(!vresponse["code"].canConvert(QVariant::Int))
			{
				*ok = false;
				return AcceptRequest::ResponseData();
			}

			out.response.code = vresponse["code"].toInt();
		}

		if(vresponse.contains("reason"))
		{
			if(vresponse["reason"].type() != QVariant::ByteArray)
			{
				*ok = false;
				return AcceptRequest::ResponseData();
			}

			out.response.reason = vresponse["reason"].toByteArray();
		}

		if(vresponse.contains("headers"))
		{
			if(vresponse["headers"].type() != QVariant::List)
			{
				*ok = false;
				return AcceptRequest::ResponseData();
			}

			foreach(const QVariant &i, vresponse["headers"].toList())
			{
				QVariantList list = i.toList();
				if(list.count() != 2)
				{
					*ok = false;
					return AcceptRequest::ResponseData();
				}

				if(list[0].type() != QVariant::ByteArray || list[1].type() != QVariant::ByteArray)
				{
					*ok = false;
					return AcceptRequest::ResponseData();
				}

				out.response.headers += QPair<QByteArray, QByteArray>(list[0].toByteArray(), list[1].toByteArray());
			}
		}

		if(vresponse.contains("body"))
		{
			if(vresponse["body"].type() != QVariant::ByteArray)
			{
				*ok = false;
				return AcceptRequest::ResponseData();
			}

			out.response.body = vresponse["body"].toByteArray();
		}
	}

	*ok = true;
	return out;
}

class AcceptRequest::Private : public QObject
{
	Q_OBJECT

public:
	AcceptRequest *q;
	ResponseData result;

	Private(AcceptRequest *_q) :
		QObject(_q),
		q(_q)
	{
	}
};

AcceptRequest::AcceptRequest(ZrpcManager *manager, QObject *parent) :
	ZrpcRequest(manager, parent)
{
	d = new Private(this);
}

AcceptRequest::~AcceptRequest()
{
	delete d;
}

AcceptRequest::ResponseData AcceptRequest::result() const
{
	return d->result;
}

void AcceptRequest::start(const AcceptData &adata)
{
	ZrpcRequest::start("accept", acceptDataToVariant(adata).toHash());
}

void AcceptRequest::onSuccess()
{
	bool ok;
	d->result = convertResult(ZrpcRequest::result(), &ok);
	if(!ok)
	{
		setError(ErrorFormat);
		return;
	}
}

#include "acceptrequest.moc"
