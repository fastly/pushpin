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

enum Scheme {
	none,
	http,
	websocket
};

// cache client params
struct CacheClientItem {
	QString connectPath;
	pid_t processId;
	bool initFlag;
	int msgIdCount;
	int requestSeqCount;
	int responseSeqCount;
	time_t lastDataReceivedTime;
	QByteArray receiver;
	QByteArray from;
	QByteArray clientId;
};

// Cache key item
enum ItemFlag {
	JSON_VALUE,
	JSON_PAIR,
	RAW_VALUE
};
struct CacheKeyItem {
	QString keyName;
	ItemFlag flag;
};

bool is_cache_method(QString methodStr);
bool is_cacheclient_inited(QList<CacheClientItem> &cacheClientList);
int get_cacheclient_no_from_response(QByteArray packetId, QList<CacheClientItem> &cacheClientList);
int get_cacheclient_no_from_init_request(ZhttpRequestPacket &p);
pid_t create_process_for_cacheclient(QString connectPath, int _no);
int select_main_cacheclient(QList<CacheClientItem> &cacheClientList);

void parse_json_map(QVariantMap& jsonData, QString keyName, QVariantMap& jsonMap);
int parse_json_msg(QVariant jsonMsg, QVariantMap& jsonMap);

void replace_id_field(QByteArray &body, QString oldId, int newId);
void replace_id_field(QByteArray &body, int oldId, QString newId);
void replace_result_field(QByteArray &body, QString oldResult, QString newResult);
void replace_subscription_field(QByteArray &body, QString oldSubscription, QString newSubscription);

QByteArray calculate_response_hash_val(QByteArray &responseBody, int idVal);
QByteArray calculate_response_seckey_from_init_request(ZhttpRequestPacket &p);

QByteArray build_hash_key(QVariantMap &jsonMap, QString startingStr);

int check_multi_packets_for_ws_request(ZhttpRequestPacket &p);

#endif
