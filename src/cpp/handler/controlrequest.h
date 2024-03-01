/*
 * Copyright (C) 2016-2017 Fanout, Inc.
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

#ifndef CONTROLREQUEST_H
#define CONTROLREQUEST_H

#include "cidset.h"
#include <boost/signals2.hpp>

using Connection = boost::signals2::scoped_connection;

class QObject;

class ZrpcManager;
class StatsPacket;
class Deferred;

namespace ControlRequest {

std::unique_ptr<Deferred> connCheck(ZrpcManager *controlClient, const CidSet &cids);
std::unique_ptr<Deferred> refresh(ZrpcManager *controlClient, const QByteArray &cid);
std::unique_ptr<Deferred> report(ZrpcManager *controlClient, const StatsPacket &packet);

}

#endif
