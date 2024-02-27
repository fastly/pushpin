/*
 * Copyright (C) 2017-2020 Fanout, Inc.
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

#include "refreshworker.h"

#include "qtcompat.h"
#include "zrpcrequest.h"
#include "controlrequest.h"
#include "statsmanager.h"
#include "wssession.h"

RefreshWorker::RefreshWorker(ZrpcRequest *req, ZrpcManager *proxyControlClient, QHash<QString, QSet<WsSession*> > *wsSessionsByChannel) :
	Deferred(),
	ignoreErrors_(false),
	proxyControlClient_(proxyControlClient),
	req_(req)
{
	req_->setParent(this);

	QVariantHash args = req_->args();

	if(args.contains("cid"))
	{
		if(typeId(args["cid"]) != QMetaType::QByteArray)
		{
			respondError("bad-request");
			return;
		}

		cids_ += QString::fromUtf8(args["cid"].toByteArray());

		refreshNextCid();
	}
	else if(args.contains("channel"))
	{
		if(typeId(args["channel"]) != QMetaType::QByteArray)
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

	Deferred *d = ControlRequest::refresh(proxyControlClient_, cids_.takeFirst().toUtf8());
	finishedConnection_ = d->finished.connect(boost::bind(&RefreshWorker::proxyRefresh_finished, this, boost::placeholders::_1));
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
