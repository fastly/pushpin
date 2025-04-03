/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#ifndef SESSIONREQUEST_H
#define SESSIONREQUEST_H

#include <QString>
#include <QList>
#include <QHash>
#include "lastids.h"
#include <boost/signals2.hpp>

using Connection = boost::signals2::scoped_connection;

class ZrpcManager;
class Deferred;
class DetectRule;

namespace SessionRequest {

Deferred *detectRulesSet(ZrpcManager *stateClient, const QList<DetectRule> &rules);
Deferred *detectRulesGet(ZrpcManager *stateClient, const QString &domain, const QByteArray &path);
Deferred *createOrUpdate(ZrpcManager *stateClient, const QString &sid, const LastIds &lastIds);
Deferred *updateMany(ZrpcManager *stateClient, const QHash<QString, LastIds> &sidLastIds);
Deferred *getLastIds(ZrpcManager *stateClient, const QString &sid);

}

#endif
