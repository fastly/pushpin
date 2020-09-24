/*
 * Copyright (C) 2017-2020 Fanout, Inc.
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

#include "refreshworker.h"

#include "zrpcrequest.h"
#include "controlrequest.h"
#include "statsmanager.h"
#include "wssession.h"

RefreshWorker::RefreshWorker(ZrpcRequest *req, ZrpcManager *proxyControlClient, QHash<QString, QSet<WsSession*> > *wsSessionsByChannel, QObject *parent) :
	Deferred(parent),
	ignoreErrors_(false),
	proxyControlClient_(proxyControlClient),
	req_(req)
{
	req_->setParent(this);

	QVariantHash args = req_->args();

	if(args.contains("cid"))
	{
		if(args["cid"].type() != QVariant::ByteArray)
		{
			respondError("bad-request");
			return;
		}

		cids_ += QString::fromUtf8(args["cid"].toByteArray());

		refreshNextCid();
	}
	else if(args.contains("channel"))
	{
		if(args["channel"].type() != QVariant::ByteArray)
		{
			respondError("bad-request");
			return;
		}

		QString channel = QString::fromUtf8(args["channel"].toByteArray());

		QSet<WsSession*> wsbc = wsSessionsByChannel->value(channel);
		foreach(WsSession *s, wsbc)
		{
			cids_ += s->cid;
		}

		ignoreErrors_ = true;

		refreshNextCid();
	}
	else
	{
		respondError("bad-request");
		return;
	}
}

void RefreshWorker::respondError(const QByteArray &condition)
{
	req_->respondError(condition);
	setFinished(true);
}

void RefreshWorker::refreshNextCid()
{
	if(cids_.isEmpty())
	{
		req_->respond();
		setFinished(true);
		return;
	}

	Deferred *d = ControlRequest::refresh(proxyControlClient_, cids_.takeFirst().toUtf8(), this);
	connect(d, &Deferred::finished, this, &RefreshWorker::proxyRefresh_finished);
}

void RefreshWorker::proxyRefresh_finished(const DeferredResult &result)
{
	if(result.success || ignoreErrors_)
	{
		refreshNextCid();
	}
	else
	{
		respondError(result.value.toByteArray());
	}
}
