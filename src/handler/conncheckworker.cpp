/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "conncheckworker.h"

#include "zrpcrequest.h"
#include "controlrequest.h"
#include "statsmanager.h"

ConnCheckWorker::ConnCheckWorker(ZrpcRequest *req, ZrpcManager *proxyControlClient, StatsManager *stats, QObject *parent) :
	Deferred(parent),
	req_(req)
{
	req_->setParent(this);

	QVariantHash args = req_->args();

	if(!args.contains("ids") || args["ids"].type() != QVariant::List)
	{
		respondError("bad-request");
		return;
	}

	QVariantList vids = args["ids"].toList();

	foreach(const QVariant &vid, vids)
	{
		if(vid.type() != QVariant::ByteArray)
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
		Deferred *d = ControlRequest::connCheck(proxyControlClient, missing_, this);
		connect(d, &Deferred::finished, this, &ConnCheckWorker::proxyConnCheck_finished);
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
