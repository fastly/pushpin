/*
 * Copyright (C) 2017 Fanout, Inc.
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

#ifndef CACHEUTIL_H
#define CACHEUTIL_H

#include <QHostAddress>
#include <QObject>
#include "zhttprequestpacket.h"
#include "zwebsocket.h"
#include "log.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"

int cacheclient_get_no(ZhttpRequestPacket &p);
pid_t cacheclient_create_child_process(QString connectPath, int _no);
void parse_json_map(QVariantMap& jsonData, QString keyName, QVariantMap& jsonMap);
int parse_json_msg(QVariant jsonMsg, QVariantMap& jsonMap);

#endif
