/*
 * Copyright (C) 2016 Fanout, Inc.
 *
 * This file is part of Pushpin.
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
 */

#include "controlrequest.h"

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
		connect(req, SIGNAL(finished()), SLOT(req_finished()));

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

class RouteRemoveAll : public Deferred
{
	Q_OBJECT

public:
	RouteRemoveAll(ZrpcManager *controlClient, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(controlClient, this);
		connect(req, SIGNAL(finished()), SLOT(req_finished()));

		req->start("route-remove-all");
	}

private slots:
	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

		if(req->success())
		{
			setFinished(true);
		}
		else
		{
			setFinished(false, req->errorCondition());
		}
	}
};

class RouteSet : public Deferred
{
	Q_OBJECT

public:
	RouteSet(ZrpcManager *controlClient, const QString &targetHost, int targetPort, bool targetSsl, bool targetOverHttp, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(controlClient, this);
		connect(req, SIGNAL(finished()), SLOT(req_finished()));

		QVariantHash args;
		args["target-host"] = targetHost.toUtf8();
		args["target-port"] = targetPort;
		args["target-ssl"] = targetSsl;
		args["target-over-http"] = targetOverHttp;
		req->start("route-set", args);
	}

private slots:
	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

		if(req->success())
		{
			setFinished(true);
		}
		else
		{
			setFinished(false, req->errorCondition());
		}
	}
};

Deferred *connCheck(ZrpcManager *controlClient, const CidSet &cids, QObject *parent)
{
	return new ConnCheck(controlClient, cids, parent);
}

Deferred *routeRemoveAll(ZrpcManager *controlClient, QObject *parent)
{
	return new RouteRemoveAll(controlClient, parent);
}

Deferred *routeSet(ZrpcManager *controlClient, const QString &targetHost, int targetPort, bool targetSsl, bool overHttp, QObject *parent)
{
	return new RouteSet(controlClient, targetHost, targetPort, targetSsl, overHttp, parent);
}

}

#include "controlrequest.moc"
