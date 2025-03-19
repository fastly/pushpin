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
#include "zhttpresponsepacket.h"
#include "zwebsocket.h"
#include "log.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"

#define AUTO_REFRESH_SHORTER_TIMEOUT	0x01
#define AUTO_REFRESH_LONGER_TIMEOUT		0x02
#define AUTO_REFRESH_NO_DELETE			0x04
#define AUTO_REFRESH_NO_REFRESH			0x08
#define AUTO_REFRESH_PATH_THROUGH		0x10

#define HTTP_REFRESH_HEADER				"HTTP_REFRESH_REQUEST"

enum Scheme {
	none,
	http,
	websocket
};

// cache client params
struct ClientItem {
	QString urlPath;
	pid_t processId;
	bool initFlag;
	QString resultStr;
	int msgIdCount;
	int lastRequestSeq;
	int lastResponseSeq;
	time_t lastRequestTime;
	time_t lastResponseTime;
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
enum CacheMethodType {
	CACHE_METHOD,
	SUBSCRIBE_METHOD
};

bool is_cache_method(QString methodStr);
bool is_subscribe_method(QString methodStr);
int get_cc_index_from_clientId(QByteArray packetId);
int get_cc_index_from_init_request(ZhttpRequestPacket &p);
pid_t create_process_for_cacheclient(QString urlPath, int _no);

int get_main_cc_index();

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
int check_multi_packets_for_ws_response(ZhttpResponsePacket &p);

int update_request_seq(const QByteArray &clientId);
int update_response_seq(const QByteArray &clientId);

void send_http_post_request(QString backend, QByteArray data);

#endif
