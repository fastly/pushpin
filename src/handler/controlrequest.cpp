/*
 * Copyright (C) 2016-2017 Fanout, Inc.
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
