/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "conncheckworker.h"

#include "qtcompat.h"
#include "zrpcrequest.h"
#include "controlrequest.h"
#include "statsmanager.h"

ConnCheckWorker::ConnCheckWorker(ZrpcRequest *req, ZrpcManager *proxyControlClient, StatsManager *stats) :
	Deferred(),
	req_(req)
{
	req_->setParent(this);

	QVariantHash args = req_->args();

	if(!args.contains("ids") || typeId(args["ids"]) != QMetaType::QVariantList)
	{
		respondError("bad-request");
		return;
	}

	QVariantList vids = args["ids"].toList();

	foreach(const QVariant &vid, vids)
	{
		if(typeId(vid) != QMetaType::QByteArray)
		{
			respondError("bad-request");
			return;
		}

		cids_ += QString::fromUtf8(vid.toByteArray());
	}

	foreach(const QString &cid, cids_)
	{
		if(!stats->checkConnection(cid.toUtf8()))
			missing_ += cid;
	}

	if(!missing_.isEmpty())
	{
		// ask the proxy about any cids we don't know about
		Deferred *d = ControlRequest::connCheck(proxyControlClient, missing_);
		finishedConnection_ = d->finished.connect(boost::bind(&ConnCheckWorker::proxyConnCheck_finished, this, boost::placeholders::_1));
		return;
	}

	doFinish();
}

void ConnCheckWorker::respondError(const QByteArray &condition)
{
	req_->respondError(condition);
	setFinished(true);
}

void ConnCheckWorker::doFinish()
{
	foreach(const QString &cid, missing_)
		cids_.remove(cid);

	QVariantList result;
	foreach(const QString &cid, cids_)
		result += cid.toUtf8();

	req_->respond(result);
	setFinished(true);
}

void ConnCheckWorker::proxyConnCheck_finished(const DeferredResult &result)
{
	if(result.success)
	{
		CidSet found = result.value.value<CidSet>();

		foreach(const QString &cid, found)
			missing_.remove(cid);

		doFinish();
	}
	else
	{
		respondError("proxy-request-failed");
	}
}
