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

#ifndef SESSIONREQUEST_H
#define SESSIONREQUEST_H

#include <QString>
#include <QList>
#include <QHash>
#include "lastids.h"

class QObject;

class ZrpcManager;
class Deferred;
class DetectRule;

namespace SessionRequest {

Deferred *detectRulesSet(ZrpcManager *stateClient, const QList<DetectRule> &rules, QObject *parent = 0);
Deferred *detectRulesGet(ZrpcManager *stateClient, const QString &domain, const QByteArray &path, QObject *parent = 0);
Deferred *createOrUpdate(ZrpcManager *stateClient, const QString &sid, const LastIds &lastIds, QObject *parent = 0);
Deferred *updateMany(ZrpcManager *stateClient, const QHash<QString, LastIds> &sidLastIds, QObject *parent = 0);
Deferred *getLastIds(ZrpcManager *stateClient, const QString &sid, QObject *parent = 0);

}

#endif
