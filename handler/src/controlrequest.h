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

#ifndef CONTROLREQUEST_H
#define CONTROLREQUEST_H

#include "cidset.h"

class QObject;

class ZrpcManager;
class Deferred;

namespace ControlRequest {

Deferred *connCheck(ZrpcManager *controlClient, const CidSet &cids, QObject *parent = 0);
Deferred *routeRemoveAll(ZrpcManager *controlClient, QObject *parent = 0);
Deferred *routeSet(ZrpcManager *controlClient, const QString &targetHost, int targetPort, bool targetSsl, bool targetOverHttp, QObject *parent = 0);

}

#endif
