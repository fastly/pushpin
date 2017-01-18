/*
 * Copyright (C) 2017 Fanout, Inc.
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

#ifndef REFRESHWORKER_H
#define REFRESHWORKER_H

#include <QByteArray>
#include "deferred.h"

class ZrpcRequest;
class ZrpcManager;
class StatsManager;

class RefreshWorker : public Deferred
{
	Q_OBJECT

public:
	RefreshWorker(ZrpcRequest *req, ZrpcManager *proxyControlClient, QObject *parent = 0);

private:
	ZrpcRequest *req_;

	void respondError(const QByteArray &condition);

private slots:
	void proxyRefresh_finished(const DeferredResult &result);
};

#endif
