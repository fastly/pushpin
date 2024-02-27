/*
 * Copyright (C) 2016-2017 Fanout, Inc.
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

#include "controlrequest.h"

#include "packet/statspacket.h"
#include "qtcompat.h"
#include "deferred.h"
#include "zrpcrequest.h"

namespace ControlRequest {

class ConnCheck : public Deferred
{
	Q_OBJECT

	Connection finishedConnection;

public:
	ConnCheck(ZrpcManager *controlClient, const CidSet &cids) :
		Deferred()
	{
		ZrpcRequest *req = new ZrpcRequest(controlClient, this);
		finishedConnection = req->finished.connect(boost::bind(&ConnCheck::req_finished, this, req));

		QVariantList vcids;
		foreach(const QString &cid, cids)
			vcids += cid.toUtf8();

		QVariantHash args;
		args["ids"] = vcids;
		req->start("conncheck", args);
	}

private:
	void req_finished(ZrpcRequest *req)
	{
		if(req->success())
		{
			QVariant vresult = req->result();
			if(typeId(vresult) != QMetaType::QVariantList)
			{
				setFinished(false);
				return;
			}

			QVariantList result = vresult.toList();

			CidSet out;
			foreach(const QVariant &vcid, result)
			{
				if(typeId(vcid) != QMetaType::QByteArray)
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

	Connection finishedConnection;

public:
	Refresh(ZrpcManager *controlClient, const QByteArray &cid) :
		Deferred()
	{
		ZrpcRequest *req = new ZrpcRequest(controlClient, this);
		finishedConnection = req->finished.connect(boost::bind(&Refresh::req_finished, this, req));

		QVariantHash args;
		args["cid"] = cid;
		req->start("refresh", args);
	}

	void req_finished(ZrpcRequest *req)
	{
		if(req->success())
			setFinished(true);
		else
			setFinished(false, req->errorConditionString());
	}
};

class Report : public Deferred
{
	Q_OBJECT

	Connection finishedConnection;

public:
	Report(ZrpcManager *controlClient, const StatsPacket &packet) :
		Deferred()
	{
		ZrpcRequest *req = new ZrpcRequest(controlClient, this);
		finishedConnection = req->finished.connect(boost::bind(&Report::req_finished, this, req));

		QVariantHash args;
		args["stats"] = packet.toVariant();
		req->start("report", args);
	}

	void req_finished(ZrpcRequest *req)
	{
		if(req->success())
			setFinished(true);
		else
			setFinished(false, req->errorCondition());
	}
};

Deferred *connCheck(ZrpcManager *controlClient, const CidSet &cids)
{
	return new ConnCheck(controlClient, cids);
}

Deferred *refresh(ZrpcManager *controlClient, const QByteArray &cid)
{
	return new Refresh(controlClient, cid);
}

Deferred *report(ZrpcManager *controlClient, const StatsPacket &packet)
{
	return new Report(controlClient, packet);
}

}

#include "controlrequest.moc"
