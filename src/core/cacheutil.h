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
#include <hiredis.h>
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "zwebsocket.h"
#include "log.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"

#define AUTO_REFRESH_SHORTER_TIMEOUT	0x01
#define AUTO_REFRESH_LONGER_TIMEOUT		0x02
#define AUTO_REFRESH_NEVER_TIMEOUT		0x04
#define AUTO_REFRESH_UNERASE			0x08
#define AUTO_REFRESH_EXCLUDE			0x10
#define AUTO_REFRESH_PASSTHROUGH		0x20

#define HTTP_REFRESH_HEADER		"HTTP_REFRESH_REQUEST"

#define RETRY_RESPONSE_MAX_COUNT	5

enum Scheme {
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

struct ClientInCacheItem {
	QString msgId;
	QByteArray from;
};

// Cache Item
struct CacheItem {
	QString orgMsgId;
	int msgId;
	int newMsgId;
	char refreshFlag;
	qint64 lastRequestTime;
	qint64 lastRefreshTime;
	qint64 lastAccessTime;
	bool cachedFlag;
	Scheme proto;
	int retryCount;
	int httpBackendNo;
	QByteArray cacheClientId;
	QString methodName;
	ZhttpRequestPacket requestPacket;
	ZhttpResponsePacket responsePacket;
	QByteArray responseHashVal;
	CacheMethodType methodType;
	QString orgSubscriptionStr;
	QString subscriptionStr;
	ZhttpResponsePacket subscriptionPacket;
	QMap<QByteArray, ClientInCacheItem> clientMap;
};

struct UnsubscribeRequestItem {
	QString subscriptionStr;
	QByteArray from;
	QString unsubscribeMethodName;
	QByteArray cacheClientId;
};

struct PacketMsg {
	QString id;
	QString method;
	QString result;
	bool isResultNull;
	QString params;
	QByteArray paramsHash;
	QString subscription;
};

void pause_cache_thread();
void resume_cache_thread();
void cache_thread();

void storeCacheItem(redisContext* context, const QByteArray& itemId, const CacheItem& item);
template <typename T>
void storeCacheItemField(redisContext* context, const QByteArray& itemId, const char *fieldName, const T& value);
CacheItem loadCacheItem(redisContext* context, const QByteArray& itemId);
template <typename T>
int loadCacheItemField(redisContext* context, const QByteArray& itemId, const char *fieldName, T& value);

bool is_convertible_to_int(const QString &str);
bool is_cache_method(QString methodStr);
bool is_subscribe_method(QString methodStr);
bool is_never_timeout_method(QString methodStr, QString paramsStr);

pid_t create_process_for_cacheclient(QString urlPath, int _no);

int get_main_cc_index();
int get_cc_index_from_clientId(QByteArray clientId);
int get_cc_index_from_init_request(ZhttpRequestPacket &p);
int get_cc_next_index_from_clientId(QByteArray clientId);

void parse_json_map(QVariantMap& jsonData, QString keyName, QVariantMap& jsonMap);
int parse_json_msg(QVariant jsonMsg, QVariantMap& jsonMap);
int parse_packet_msg(Scheme scheme, const ZhttpRequestPacket& packet, PacketMsg& packetMsg);
int parse_packet_msg(Scheme scheme, const ZhttpResponsePacket& packet, PacketMsg& packetMsg);

void replace_id_field(QByteArray &body, QString oldId, int newId);
void replace_id_field(QByteArray &body, QString oldId, QString newId);
void replace_id_field(QByteArray &body, int oldId, QString newId);
void replace_result_field(QByteArray &body, QString oldResult, QString newResult);
void replace_subscription_field(QByteArray &body, QString oldSubscription, QString newSubscription);

QByteArray calculate_response_hash_val(QByteArray &responseBody, int idVal);
QByteArray calculate_response_seckey_from_init_request(ZhttpRequestPacket &p);

QByteArray build_hash_key(QVariantMap &jsonMap, QString startingStr);

int check_multi_packets_for_ws_request(ZhttpRequestPacket &p);
int check_multi_packets_for_ws_response(ZhttpResponsePacket &p);

int update_request_seq(const QByteArray &clientId);
int get_client_new_response_seq(const QByteArray &clientId);
void update_client_response_seq(const QByteArray &clientId, int seqNum);

void send_http_post_request_with_refresh_header(QString backend, QByteArray postData, char *headerVal);

int get_next_cache_refresh_interval(const QByteArray &itemId);

QString get_switched_http_backend_url(QString currUrl);
QString get_switched_ws_backend_url(QString currUrl);

#endif
