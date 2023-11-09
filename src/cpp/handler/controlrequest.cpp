/*
 * Copyright (C) 2016-2017 Fanout, Inc.
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

#include "controlrequest.h"

#include "packet/statspacket.h"
#include "deferred.h"
#include "zrpcrequest.h"

namespace ControlRequest {

class ConnCheck : public Deferred
{
	Q_OBJECT

public:
	ConnCheck(ZrpcManager *controlClient, const CidSet &cids, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(controlClient, this);
		connect(req, &ZrpcRequest::finished, this, &ConnCheck::req_finished);

		QVariantList vcids;
		foreach(const QString &cid, cids)
			vcids += cid.toUtf8();

		QVariantHash args;
		args["ids"] = vcids;
		req->start("conncheck", args);
	}

private slots:
	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

		if(req->success())
		{
			QVariant vresult = req->result();
			if(vresult.type() != QVariant::List)
			{
				setFinished(false);
				return;
			}

			QVariantList result = vresult.toList();

			CidSet out;
			foreach(const QVariant &vcid, result)
			{
				if(vcid.type() != QVariant::ByteArray)
				{
					setFinished(false);
					return;
				}

				out += QString::fromUtf8(vcid.toByteArray());
			}

			setFinished(true, QVariant::fromValue<CidSet>(out));
		}
		else
		{
			setFinished(false, req->errorCondition());
		}
	}
};

class Refresh : public Deferred
{
	Q_OBJECT

public:
	Refresh(ZrpcManager *controlClient, const QByteArray &cid, QObject *parent) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(controlClient, this);
		connect(req, &ZrpcRequest::finished, this, &Refresh::req_finished);

		QVariantHash args;
		args["cid"] = cid;
		req->start("refresh", args);
	}

	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

		if(req->success())
			setFinished(true);
		else
			setFinished(false, req->errorConditionString());
	}
};

class Report : public Deferred
{
	Q_OBJECT

public:
	Report(ZrpcManager *controlClient, const StatsPacket &packet, QObject *parent) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(controlClient, this);
		connect(req, &ZrpcRequest::finished, this, &Report::req_finished);

		QVariantHash args;
		args["stats"] = packet.toVariant();
		req->start("report", args);
	}

	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

		if(req->success())
			setFinished(true);
		else
			setFinished(false, req->errorCondition());
	}
};

Deferred *connCheck(ZrpcManager *controlClient, const CidSet &cids, QObject *parent)
{
	return new ConnCheck(controlClient, cids, parent);
}

Deferred *refresh(ZrpcManager *controlClient, const QByteArray &cid, QObject *parent)
{
	return new Refresh(controlClient, cid, parent);
}

Deferred *report(ZrpcManager *controlClient, const StatsPacket &packet, QObject *parent)
{
	return new Report(controlClient, packet, parent);
}

}

#include "controlrequest.moc"