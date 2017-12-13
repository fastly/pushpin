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

#ifndef CONTROLREQUEST_H
#define CONTROLREQUEST_H

#include "cidset.h"

class QObject;

class ZrpcManager;
class StatsPacket;
class Deferred;

namespace ControlRequest {

Deferred *connCheck(ZrpcManager *controlClient, const CidSet &cids, QObject *parent = 0);
Deferred *refresh(ZrpcManager *controlClient, const QByteArray &cid, QObject *parent = 0);
Deferred *report(ZrpcManager *controlClient, const StatsPacket &packet, QObject *parent = 0);

}

#endif
