/*
 * Copyright (C) 2015-2023 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
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

#include "acceptrequest.h"

#include "qtcompat.h"
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

			if(r.unreportedTime > 0)
				vrequest["unreported-time"] = r.unreportedTime;

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

	if(adata.trusted)
		obj["trusted"] = true;

	if(adata.useSession)
		obj["use-session"] = true;

	if(adata.responseSent)
		obj["response-sent"] = true;

	if(!adata.connMaxPackets.isEmpty())
		obj["conn-max"] = adata.connMaxPackets;

	return obj;
}

static AcceptRequest::ResponseData convertResult(const QVariant &in, bool *ok)
{
	AcceptRequest::ResponseData out;

	if(typeId(in) != QMetaType::QVariantHash)
	{
		*ok = false;
		return AcceptRequest::ResponseData();
	}

	QVariantHash obj = in.toHash();

	if(obj.contains("accepted"))
	{
		if(typeId(obj["accepted"]) != QMetaType::Bool)
		{
			*ok = false;
			return AcceptRequest::ResponseData();
		}

		out.accepted = obj["accepted"].toBool();
	}

	if(obj.contains("response"))
	{
		if(typeId(obj["response"]) != QMetaType::QVariantHash)
		{
			*ok = false;
			return AcceptRequest::ResponseData();
		}

		QVariantHash vresponse = obj["response"].toHash();

		if(vresponse.contains("code"))
		{
			if(!canConvert(vresponse["code"], QMetaType::Int))
			{
				*ok = false;
				return AcceptRequest::ResponseData();
			}

			out.response.code = vresponse["code"].toInt();
		}

		if(vresponse.contains("reason"))
		{
			if(typeId(vresponse["reason"]) != QMetaType::QByteArray)
			{
				*ok = false;
				return AcceptRequest::ResponseData();
			}

			out.response.reason = vresponse["reason"].toByteArray();
		}

		if(vresponse.contains("headers"))
		{
			if(typeId(vresponse["headers"]) != QMetaType::QVariantList)
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

				if(typeId(list[0]) != QMetaType::QByteArray || typeId(list[1]) != QMetaType::QByteArray)
				{
					*ok = false;
					return AcceptRequest::ResponseData();
				}

				out.response.headers += QPair<QByteArray, QByteArray>(list[0].toByteArray(), list[1].toByteArray());
			}
		}

		if(vresponse.contains("body"))
		{
			if(typeId(vresponse["body"]) != QMetaType::QByteArray)
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
