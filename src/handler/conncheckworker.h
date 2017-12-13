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

#ifndef CONNCHECKWORKER_H
#define CONNCHECKWORKER_H

#include <QByteArray>
#include "deferred.h"
#include "cidset.h"

class ZrpcRequest;
class ZrpcManager;
class StatsManager;

class ConnCheckWorker : public Deferred
{
	Q_OBJECT

public:
	ConnCheckWorker(ZrpcRequest *req, ZrpcManager *proxyControlClient, StatsManager *stats, QObject *parent = 0);

private:
	ZrpcRequest *req_;
	CidSet cids_;
	CidSet missing_;

	void respondError(const QByteArray &condition);
	void doFinish();

private slots:
	void proxyConnCheck_finished(const DeferredResult &result);
};

#endif
