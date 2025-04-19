/*
 * Copyright (C) 2017-2022 Fanout, Inc.
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

#include "cacheutil.h"

#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <QHash>
#include <QUuid>
#include <QSettings>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QDebug>
#include <QCryptographicHash>
#include <QThread>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QElapsedTimer>
#include <QDataStream>

#include "qtcompat.h"
#include "tnetstring.h"
#include "log.h"

extern bool gCacheEnable;
extern QStringList gHttpBackendUrlList;
extern QStringList gWsBackendUrlList;

QMap<QByteArray, CacheItem> gCacheItemMap;

unsigned long long numRequestMultiPart = 0;

extern QString gMsgIdAttrName;
extern QString gMsgMethodAttrName;
extern QString gMsgParamsAttrName;
extern QString gResultAttrName;
extern QString gSubscriptionAttrName;
extern QString gSubscribeBlockAttrName;
extern QString gSubscribeChangesAttrName;

extern QStringList gCacheMethodList;
extern QMap<QString, QString> gSubscribeMethodMap;
extern QList<UnsubscribeRequestItem> gUnsubscribeRequestList;
extern QStringList gNeverTimeoutMethodList;
extern QList<CacheKeyItem> gCacheKeyItemList;

// multi packets params
extern ZhttpResponsePacket gHttpMultiPartResponsePacket;
extern QMap<QByteArray, ZhttpRequestPacket> gWsMultiPartRequestItemMap;
extern QMap<QByteArray, ZhttpResponsePacket> gWsMultiPartResponseItemMap;

extern QList<ClientItem> gWsCacheClientList;
extern QMap<QByteArray, ClientItem> gWsClientMap;
extern QMap<QByteArray, ClientItem> gHttpClientMap;

extern int gAccessTimeoutSeconds;
extern int gResponseTimeoutSeconds;
extern int gCacheTimeoutSeconds;
extern int gShorterTimeoutSeconds;
extern int gLongerTimeoutSeconds;
extern int gCacheItemMaxCount;

// redis
extern redisContext *gRedisContext;
extern bool gRedisEnable;

// definitions for cache
#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define REDIS_CACHE_ID_HEADER	"PUSHPIN : "

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// HiRedis

redisContext* connectToRedis() 
{
	const char *hostname = "127.0.0.1";
	int port = 6379;
	struct timeval timeout = {1, 500000}; // 1.5 seconds
	redisContext *c = redisConnectWithTimeout(hostname, port, timeout);
	if (c == nullptr || c->err) 
	{
		if (c) 
		{
			log_debug("Connection error: %s", c->errstr);
			redisFree(c);
		} 
		else 
		{
			log_debug("Connection error: can't allocate redis context");
		}
		return nullptr;
	}
	log_debug("[CACHE] Connected to redis server %s:%d", hostname, port);

	redis_removeall_cache_item(c);
	return c;
}

bool redis_is_cache_item(redisContext* context, const QByteArray& itemId)
{
	bool ret = false;

	if (context == nullptr)
		return ret;

	QByteArray key = REDIS_CACHE_ID_HEADER + itemId;

	redisReply *reply = (redisReply*)redisCommand(context, "EXISTS %b", key.constData(), key.size());
	if (reply == NULL) 
	{
		log_debug("[REDIS] EXISTS Command failed\n");
		return ret;
	}

	if (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) 
	{
		ret = true;
	}

	if (reply) freeReplyObject(reply);
	
	return ret;
}

void redis_save_cache_item(redisContext* context, const QByteArray& itemId, const CacheItem& item) 
{
	if (context == nullptr)
		return;

	QByteArray key = REDIS_CACHE_ID_HEADER + itemId;

	QByteArray requestPacket = TnetString::fromVariant(item.requestPacket.toVariant());
	QByteArray responsePacket = TnetString::fromVariant(item.responsePacket.toVariant());
	QByteArray subscriptionPacket = TnetString::fromVariant(item.subscriptionPacket.toVariant());

	redisReply* reply = (redisReply*)redisCommand(context,
		"HSET %b "
		"orgMsgId %b "
		"msgId %d "
		"newMsgId %d "
		"refreshFlag %d "
		"lastRequestTime %lld "
		"lastRefreshTime %lld "
		"lastAccessTime %lld "
		"cachedFlag %d "
		"proto %d "
		"retryCount %d "
		"httpBackendNo %d "
		"cacheClientId %b "
		"methodName %b "
		"requestPacket %b "
		"responsePacket %b "
		"responseHashVal %b "
		"methodType %d "
		"orgSubscriptionStr %b "
		"subscriptionStr %b "
		"subscriptionPacket %b",

		key.constData(), key.size(),

		item.orgMsgId.toUtf8().constData(), item.orgMsgId.toUtf8().size(),
		item.msgId,
		item.newMsgId,
		(int)item.refreshFlag,
		static_cast<long long>(item.lastRequestTime),
		static_cast<long long>(item.lastRefreshTime),
		static_cast<long long>(item.lastAccessTime),
		item.cachedFlag ? 1 : 0,
		item.proto,
		item.retryCount,
		item.httpBackendNo,
		item.cacheClientId.constData(), item.cacheClientId.size(),
		item.methodName.toUtf8().constData(), item.methodName.toUtf8().size(),
		requestPacket.constData(), requestPacket.size(),
		responsePacket.constData(), responsePacket.size(),
		item.responseHashVal.constData(), item.responseHashVal.size(),
		item.methodType,
		item.orgSubscriptionStr.toUtf8().constData(), item.orgSubscriptionStr.toUtf8().size(),
		item.subscriptionStr.toUtf8().constData(), item.subscriptionStr.toUtf8().size(),
		subscriptionPacket.constData(), subscriptionPacket.size()
	);

	if (reply != nullptr)
		freeReplyObject(reply);

	// store client map
	QMap<QByteArray, ClientInCacheItem> clientMap = item.clientMap;

	// delete original client map
	QString originalClientMapVal = "";
	redis_load_cache_item_field<QString>(context, itemId, "clientMap", originalClientMapVal);
	QStringList mapList = originalClientMapVal.split("\n");
	for	(int i=0; i < mapList.length(); i++)
	{
		QString mapKeyStr = mapList[i];
		if (!mapKeyStr.isEmpty())
		{
			redis_remove_cache_item_field<QString>(context, itemId, qPrintable(mapKeyStr));
		}
	}

	// store new client map
	QString newClientMapVal = "";	
	for (const QByteArray &mapKey : clientMap.keys()) 
	{
		QString keyStr = mapKey.toHex().data();
		keyStr += "\n";
		newClientMapVal += keyStr;
		QString clientItemVal = clientMap[mapKey].msgId;
		clientItemVal += "\n";
		clientItemVal += clientMap[mapKey].from.toHex().data();
		//log_debug("Store clientItemVal=%s", qPrintable(clientItemVal));
		redis_store_cache_item_field<QString>(context, itemId, mapKey.toHex().data(), clientItemVal);
	}

	//log_debug("Store newClientMapVal=%s", qPrintable(newClientMapVal));
	redis_store_cache_item_field<QString>(context, itemId, "clientMap", newClientMapVal);
}

template <typename T>
void redis_store_cache_item_field(redisContext* context, const QByteArray& itemId, const char* fieldName, const T& value) 
{
	if (context == nullptr)
		return;

	QByteArray key = REDIS_CACHE_ID_HEADER + itemId;

	redisReply* reply = nullptr;
	if constexpr (std::is_same<T, QString>::value)
	{
		reply = (redisReply*)redisCommand(context,
			"HSET %b "
			"%s %b",
			key.constData(), key.size(),
			fieldName, 
			value.toUtf8().constData(), value.toUtf8().size()
		);
	}
	else if constexpr (std::is_same<T, int>::value)
	{
		reply = (redisReply*)redisCommand(context,
			"HSET %b "
			"%s %d",
			key.constData(), key.size(),
			fieldName, 
			value
		);
	}
	else if constexpr (std::is_same<T, float>::value || std::is_same<T, double>::value)
	{
		reply = (redisReply*)redisCommand(context,
			"HSET %b "
			"%s %f",
			key.constData(), key.size(),
			fieldName, 
			value
		);
	}
	else if constexpr (std::is_same<T, bool>::value)
	{
		reply = (redisReply*)redisCommand(context,
			"HSET %b "
			"%s %d",
			key.constData(), key.size(),
			fieldName, 
			value ? 1 : 0
		);
	}
	else if constexpr (std::is_same<T, char*>::value || std::is_same<T, const char*>::value)
	{
		reply = (redisReply*)redisCommand(context,
			"HSET %b "
			"%s %s",
			key.constData(), key.size(),
			fieldName, 
			value
		);
	}
	else if constexpr (std::is_same<T, long>::value || std::is_same<T, long long>::value)
	{
		reply = (redisReply*)redisCommand(context,
			"HSET %b "
			"%s %lld",
			key.constData(), key.size(),
			fieldName, 
			value
		);
	}
	else if constexpr (std::is_same<T, QByteArray>::value)
	{
		reply = (redisReply*)redisCommand(context,
			"HSET %b "
			"%s %b",
			key.constData(), key.size(),
			fieldName, 
			value.constData(), value.size()
		);
	}
	else if constexpr (std::is_same<T, ZhttpRequestPacket>::value || std::is_same<T, ZhttpResponsePacket>::value)
	{
		QVariant vpacket = value.toVariant();
		QByteArray buf = TnetString::fromVariant(vpacket);
		reply = (redisReply*)redisCommand(context,
			"HSET %b "
			"%s %b",
			key.constData(), key.size(),
			qPrintable(fieldName), 
			buf.constData(), buf.size()
		);
	}
	else if constexpr (std::is_same<T, QMap<QByteArray, ClientInCacheItem>>::value)
	{
		QMap<QByteArray, ClientInCacheItem> clientMap = value;

		// delete original client map
		QString originalClientMapVal = "";
		redis_load_cache_item_field<QString>(context, itemId, "clientMap", originalClientMapVal);
		QStringList mapList = originalClientMapVal.split("\n");
		for	(int i=0; i < mapList.length(); i++)
		{
			QString mapKeyStr = mapList[i];
			if (!mapKeyStr.isEmpty())
			{
				redis_remove_cache_item_field<QString>(context, itemId, qPrintable(mapKeyStr));
			}
		}

		// store new client map
		QString newClientMapVal = "";	
		for (const QByteArray &mapKey : clientMap.keys()) 
		{
			QString keyStr = mapKey.toHex().data();
			keyStr += "\n";
			newClientMapVal += keyStr;
			QString clientItemVal = clientMap[mapKey].msgId;
			clientItemVal += "\n";
			clientItemVal += clientMap[mapKey].from.toHex().data();
			//log_debug("Store clientItemVal=%s", qPrintable(clientItemVal));
			redis_store_cache_item_field<QString>(context, itemId, mapKey.toHex().data(), clientItemVal);
		}

		//log_debug("Store newClientMapVal=%s", qPrintable(newClientMapVal));
		redis_store_cache_item_field<QString>(context, itemId, "clientMap", newClientMapVal);
	}

	if (reply != nullptr) 
		freeReplyObject(reply);
}

CacheItem redis_load_cache_item(redisContext* context, const QByteArray& itemId) 
{
	CacheItem item;
	QByteArray key = REDIS_CACHE_ID_HEADER + itemId;

	redisReply* reply = (redisReply*)redisCommand(context,
		"HGETALL %b", key.constData(), key.size());

	if (!reply || reply->type != REDIS_REPLY_ARRAY) {
		if (reply) freeReplyObject(reply);
		return item;
	}

	QString clientMap = "";
	for (size_t i = 0; i < reply->elements; i += 2) {
		QByteArray field(reply->element[i]->str, reply->element[i]->len);
		QByteArray value(reply->element[i + 1]->str, reply->element[i + 1]->len);

		if (field == "orgMsgId") item.orgMsgId = QString::fromUtf8(value);
		else if (field == "msgId") item.msgId = value.toInt();
		else if (field == "newMsgId") item.newMsgId = value.toInt();
		else if (field == "refreshFlag") item.refreshFlag = (char)value.toInt();
		else if (field == "lastRequestTime") item.lastRequestTime = value.toLongLong();
		else if (field == "lastRefreshTime") item.lastRefreshTime = value.toLongLong();
		else if (field == "lastAccessTime") item.lastAccessTime = value.toLongLong();
		else if (field == "cachedFlag") item.cachedFlag = (value == "1");
		else if (field == "proto") item.proto = (value == "0") ? Scheme::http : Scheme::websocket;
		else if (field == "retryCount") item.retryCount = value.toInt();
		else if (field == "httpBackendNo") item.httpBackendNo = value.toInt();
		else if (field == "cacheClientId") item.cacheClientId = value;
		else if (field == "methodName") item.methodName = QString::fromUtf8(value);
		else if (field == "requestPacket") item.requestPacket.fromVariant(TnetString::toVariant(value));
		else if (field == "responsePacket") item.responsePacket.fromVariant(TnetString::toVariant(value));
		else if (field == "responseHashVal") item.responseHashVal = value;
		else if (field == "methodType") item.methodType = (value == "0") ? CacheMethodType::CACHE_METHOD : CacheMethodType::SUBSCRIBE_METHOD;
		else if (field == "orgSubscriptionStr") item.orgSubscriptionStr = QString::fromUtf8(value);
		else if (field == "subscriptionStr") item.subscriptionStr = QString::fromUtf8(value);
		else if (field == "subscriptionPacket") item.subscriptionPacket.fromVariant(TnetString::toVariant(value));
		else if (field == "clientMap") clientMap = QString::fromUtf8(value);
	}
	freeReplyObject(reply);

	//log_debug("Load ClientMap=%s", qPrintable(clientMap));
	QStringList mapList = clientMap.split("\n");
	for	(int i=0; i < mapList.length(); i++)
	{
		QString mapKeyStr = mapList[i];
		if (!mapKeyStr.isEmpty())
		{
			QString mapValStr = "";
			redis_load_cache_item_field<QString>(context, itemId, qPrintable(mapKeyStr), mapValStr);
			//log_debug("mapValStr = %s", qPrintable(mapValStr));
			QStringList mapValList = mapValStr.split("\n");
			if (mapValList.length() == 2)
			{
				ClientInCacheItem cacheClientItem;
				cacheClientItem.msgId = mapValList[0];
				cacheClientItem.from = QByteArray::fromHex(qPrintable(mapValList[1]));
				QByteArray mapKeyByte = QByteArray::fromHex(qPrintable(mapKeyStr));
				item.clientMap[mapKeyByte] = cacheClientItem;
			}
		}
	}

	return item;
}

template <typename T>
int redis_load_cache_item_field(redisContext* context, const QByteArray& itemId, const char *fieldName, T& value) 
{
	if (context == nullptr)
		return -1;

	QByteArray key = REDIS_CACHE_ID_HEADER + itemId;

	redisReply* reply = (redisReply*)redisCommand(context,
		"HGET %b "
		"%s",
		key.constData(), key.size(),
		fieldName
	);

	if (reply == nullptr)
		return -1;

	QByteArray output(reply->str, reply->len);
	
	if constexpr (std::is_same<T, QString>::value)
		value = QString::fromUtf8(output);
	else if constexpr (std::is_same<T, int>::value)
		value = output.toInt();
	else if constexpr (std::is_same<T, float>::value)
		value = output.toFloat();
	else if constexpr (std::is_same<T, double>::value)
		value = output.toDouble();
	else if constexpr (std::is_same<T, bool>::value)
		value = (output == "1");
	else if constexpr (std::is_same<T, char*>::value)
		value = output.data();
	else if constexpr (std::is_same<T, const char*>::value)
		value = output.constData();
	else if constexpr (std::is_same<T, long>::value)
		value = output.toLong();
	else if constexpr (std::is_same<T, long long>::value)
		value = output.toLongLong();
	else if constexpr (std::is_same<T, QByteArray>::value)
		value = output;
	else if constexpr (std::is_same<T, ZhttpRequestPacket>::value)
	{
		QVariant data = TnetString::toVariant(output);
		value.fromVariant(data);
	}
	else if constexpr (std::is_same<T, ZhttpResponsePacket>::value)
	{
		QVariant data = TnetString::toVariant(output);
		value.fromVariant(data);
	}
	else if constexpr (std::is_same<T, QMap<QByteArray, ClientInCacheItem>>::value)
	{
		QString clientMap = QString::fromUtf8(output);
		log_debug("Load ClientMap=%s", qPrintable(clientMap));
		QStringList mapList = clientMap.split("\n");
		for	(int i=0; i < mapList.length(); i++)
		{
			QString mapKeyStr = mapList[i];
			if (!mapKeyStr.isEmpty())
			{
				QString mapValStr = "";
				redis_load_cache_item_field<QString>(context, itemId, qPrintable(mapKeyStr), mapValStr);
				log_debug("mapValStr = %s", qPrintable(mapValStr));
				QStringList mapValList = mapValStr.split("\n");
				if (mapValList.length() == 2)
				{
					ClientInCacheItem cacheClientItem;
					cacheClientItem.msgId = mapValList[0];
					cacheClientItem.from = QByteArray::fromHex(qPrintable(mapValList[1]));
					QByteArray mapKeyByte = QByteArray::fromHex(qPrintable(mapKeyStr));
					value[mapKeyByte] = cacheClientItem;
				}
			}
		}
	}

	if (reply != nullptr)
		freeReplyObject(reply);
	return 0;
}

void redis_remove_cache_item_field(redisContext *context, const QByteArray &itemId, const char* fieldName) 
{
	QByteArray key = REDIS_CACHE_ID_HEADER + itemId;

	redisReply* reply = (redisReply*)redisCommand(context, 
		"HDEL %b %s", 
		key.constData(), key.size(), 
		fieldName
	);
	if (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0) 
	{
		log_debug("[REDIS] removed field %s, %s", itemId.toHex().data(), fieldName);
	}
	freeReplyObject(reply);

	return;
}

void redis_remove_cache_item(redisContext *context, const QByteArray &itemId) 
{
	QByteArray key = REDIS_CACHE_ID_HEADER + itemId;

	redisReply* reply = (redisReply*)redisCommand(context,
		"DEL %b",
		key.constData(), key.size()
	);

	if (reply != nullptr)
		freeReplyObject(reply);

	return;
}

void redis_removeall_cache_item(redisContext *context) 
{
	redisReply* reply = (redisReply*)redisCommand(context, "FLUSHDB");

	if (reply->type == REDIS_REPLY_STATUS && std::string(reply->str) == "OK") 
	{
		log_debug("[REDIS] Database cleared successfully.");
	}

	if (reply != nullptr)
		freeReplyObject(reply);

	return;
}

QList<QByteArray> redis_get_cache_item_ids(redisContext *context) 
{
	QList<QByteArray> ret;

	QByteArray key = REDIS_CACHE_ID_HEADER;

	redisReply* reply = (redisReply*)redisCommand(context,
		"KEYS %b*",
		key.constData(), key.size()
	);

	if (reply == nullptr)
	{
		log_debug("[REDIS] failed to get id list");
		return ret;
	}

	if (reply->type == REDIS_REPLY_ARRAY) 
	{
		int idHeaderLen = strlen(REDIS_CACHE_ID_HEADER);
		for (size_t i = 0; i < reply->elements; i++) 
		{
			// remove REDIS_CACHE_ID_HEADER
			int eleLen = reply->element[i]->len;
			if (eleLen > idHeaderLen)
			{
				QByteArray value(reply->element[i]->str, eleLen);
				value.remove(0, idHeaderLen);
				ret.append(value);
			}
		}
	}

	freeReplyObject(reply);

	return ret;
}

void setQByteArrayToRedis(redisContext *c, const QByteArray &key, const QByteArray &value) 
{
	QByteArray field0 = "username";
	QByteArray field1 = "email";
	QByteArray value1 = "root@root.com";
	redisReply *reply = (redisReply *)redisCommand(c, "HSET %b %b %b %b %b",
		key.constData(), (size_t)key.size(),
		field0.constData(), (size_t)field0.size(),
		value.constData(), (size_t)value.size(),
		field1.constData(), (size_t)field1.size(),
		value1.constData(), (size_t)value1.size());
	if (reply == nullptr) 
	{
		log_debug("HSET command failed: %d", c->errstr);
		return;
	}
	log_debug("HSET command response: %s", reply->str);
	freeReplyObject(reply);
}

QByteArray getQByteArrayFromRedis(redisContext *c, const QByteArray &key) 
{
	QByteArray field = "email";
	redisReply *reply = (redisReply *)redisCommand(c, "HGET %b %b", 
		key.constData(), (size_t)key.size(),
		field.constData(), (size_t)field.size());
	QByteArray value;
	if (reply == nullptr) 
	{
		log_debug("HGET command failed: %d", c->errstr);
	} 
	else if (reply->type == REDIS_REPLY_STRING) 
	{
		value = QByteArray(reply->str, reply->len);
		log_debug("HGET command response: %s", value.data());
	} 
	else 
	{
		log_debug("HGET command returned unexpected type. %d", reply->type);
	}
	freeReplyObject(reply);
	return value;
}

void testRedis()
{
	redisContext *c = connectToRedis();
	if (c == nullptr) 
	{
		return;
	}
/*
	ClientItem item;
	item.clientId = "abc123";
	item.urlPath = "/do/task";
	item.processId = getpid();
	item.initFlag = true;
	item.resultStr = "ok";
	item.msgIdCount = 42;
	item.lastRequestSeq = 5;
	item.lastResponseSeq = 5;
	item.lastRequestTime = time(nullptr);
	item.lastResponseTime = time(nullptr);
	item.receiver = QByteArray::fromHex("deadbeef");
	item.from = QByteArray::fromHex("device");

	redis_save_cache_item(c, item);

	storeCacheItemField<QString>(c, item.clientId, "urlPath", "/do/update");
	storeCacheItemField<pid_t>(c, item.clientId, "processId", getpid());
	storeCacheItemField<bool>(c, item.clientId, "initFlag", true);
	storeCacheItemField<QString>(c, item.clientId, "resultStr", "okk");
	storeCacheItemField<int>(c, item.clientId, "msgIdCount", 42);
	storeCacheItemField<int>(c, item.clientId, "lastRequestSeq", 5);
	storeCacheItemField<int>(c, item.clientId, "lastResponseSeq", 5);
	storeCacheItemField<time_t>(c, item.clientId, "lastRequestTime", time(nullptr));
	storeCacheItemField<time_t>(c, item.clientId, "lastResponseTime", time(nullptr));
	storeCacheItemField<QByteArray>(c, item.clientId, "receiver", QByteArray::fromHex("1234567890"));
	storeCacheItemField<QByteArray>(c, item.clientId, "from", QByteArray::fromHex("abcdef"));
	ZhttpRequestPacket packet;
	packet.code = 2222;
	storeCacheItemField<ZhttpRequestPacket>(c, item.clientId, "requestPacket", packet);
	QMap<QByteArray, ClientInCacheItem> clientMap;
	ClientInCacheItem clientItem0;
	clientItem0.msgId = "1";
	clientItem0.from = QByteArray::fromHex("abcdef");
	QByteArray key0 = QByteArray::fromHex("123456");
	clientMap[key0] = clientItem0;
	ClientInCacheItem clientItem1;
	clientItem1.msgId = "2";
	clientItem1.from = QByteArray::fromHex("acdef");
	log_debug("PPPPP=%s", clientItem1.from.toHex().data());
	QByteArray key1 = QByteArray::fromHex("234567");
	clientMap[key1] = clientItem1;
	storeCacheItemField<QMap<QByteArray, ClientInCacheItem>>(c, item.clientId, "clientMap", clientMap);

	ClientItem newItem;
	loadCacheItemField<QString>(c, item.clientId, "urlPath", newItem.urlPath);
	loadCacheItemField<pid_t>(c, item.clientId, "processId", newItem.processId);
	loadCacheItemField<bool>(c, item.clientId, "initFlag", newItem.initFlag);
	loadCacheItemField<QString>(c, item.clientId, "resultStr", newItem.resultStr);
	loadCacheItemField<int>(c, item.clientId, "msgIdCount", newItem.msgIdCount);
	loadCacheItemField<int>(c, item.clientId, "lastRequestSeq", newItem.lastRequestSeq);
	loadCacheItemField<int>(c, item.clientId, "lastResponseSeq", newItem.lastResponseSeq);
	loadCacheItemField<time_t>(c, item.clientId, "lastRequestTime", newItem.lastRequestTime);
	loadCacheItemField<time_t>(c, item.clientId, "lastResponseTime", newItem.lastResponseTime);
	loadCacheItemField<QByteArray>(c, item.clientId, "receiver", newItem.receiver);
	loadCacheItemField<QByteArray>(c, item.clientId, "from", newItem.from);
	ZhttpRequestPacket newPacket;
	loadCacheItemField<ZhttpRequestPacket>(c, item.clientId, "requestPacket", newPacket);
	QMap<QByteArray, ClientInCacheItem> newClientMap;
	loadCacheItemField<QMap<QByteArray, ClientInCacheItem>>(c, item.clientId, "clientMap", newClientMap);

	for (const QByteArray &mapKey : newClientMap.keys())
	{
		log_debug("TTTTT %s, %s, %s", mapKey.toHex().data(), qPrintable(newClientMap[mapKey].msgId), newClientMap[mapKey].from.toHex().data());
	}

	log_debug("urlPath = %s", qPrintable(newItem.urlPath));
	log_debug("processId = %d", newItem.processId);
	log_debug("initFlag = %s", newItem.initFlag ? "true" : "false");
	log_debug("resultStr = %s", qPrintable(newItem.resultStr));
	log_debug("msgIdCount = %d", newItem.msgIdCount);
	log_debug("lastRequestSeq = %d", newItem.lastRequestSeq);
	log_debug("lastResponseSeq = %d", newItem.lastResponseSeq);
	log_debug("lastRequestTime = %lld", newItem.lastRequestTime);
	log_debug("lastResponseTime = %lld", newItem.lastResponseTime);
	log_debug("receiver = %s", newItem.receiver.toHex().data());
	log_debug("from = %s", newItem.from.toHex().data());
	log_debug("code = %d", newPacket.code);

	ClientItem loaded = loadCacheItem(c, item.clientId);
	log_debug("Loaded URL:%s", qPrintable(loaded.urlPath));
*/
	redisFree(c);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Cache Item
bool is_cache_item(const QByteArray& itemId)
{
	bool ret = false;

	if (gRedisEnable == false)
	{
		// global cache item map
		ret = gCacheItemMap.contains(itemId);
	}
	else
	{
		// redis
		ret = redis_is_cache_item(gRedisContext, itemId);
	}

	return ret;
}

CacheItem* load_cache_item(const QByteArray& itemId)
{
	CacheItem* ret = NULL;

	if (gRedisEnable == false)
	{
		// global cache item map
		if (!is_cache_item(itemId))
		{
			log_debug("[CACHE] not found cache item %s", itemId.toHex().data());
			return NULL;
		}
		ret = &gCacheItemMap[itemId];
	}
	else
	{
		// redis
		if (!redis_is_cache_item(gRedisContext, itemId))
		{
			log_debug("[REDIS] not found cache item %s", itemId.toHex().data());
			return NULL;
		}
		// store into global map and return of it`s pointer
		gCacheItemMap[itemId] = redis_load_cache_item(gRedisContext, itemId);
		ret = &gCacheItemMap[itemId];
	}

	return ret;
}

void store_cache_item(const QByteArray& itemId)
{
	if (gRedisEnable == false)
	{
		// global cache item map
		log_debug("[CACHE] store cache item %s", itemId.toHex().data());
	}
	else
	{
		// redis
		if (gCacheItemMap.contains(itemId))
		{
			log_debug("[REDIS] save cache item %s", itemId.toHex().data());
			redis_save_cache_item(gRedisContext, itemId, gCacheItemMap[itemId]);	
		}
		else
		{
			log_debug("[REDIS] not loaded cache item %s", itemId.toHex().data());
		}
	}
	
	return;
}

void store_cache_item_field(const QByteArray& itemId, const char* fieldName, const int& value)
{
	if (gRedisEnable == false)
	{
		// global cache item map
		log_debug("[CACHE] store cache item %s, %s", itemId.toHex().data(), qPrintable(fieldName));
	}
	else
	{
		// redis
		if (gCacheItemMap.contains(itemId))
		{
			log_debug("[REDIS] save cache item field %s, %s", itemId.toHex().data(), qPrintable(fieldName));
			redis_store_cache_item_field<int>(gRedisContext, itemId, fieldName, value);	
		}
		else
		{
			log_debug("[REDIS] not loaded cache item field %s, %s", itemId.toHex().data(), qPrintable(fieldName));
		}
	}
	
	return;
}

void store_cache_item_field(const QByteArray& itemId, const char* fieldName, const QByteArray& value)
{
	if (gRedisEnable == false)
	{
		// global cache item map
		log_debug("[CACHE] store cache item %s, %s", itemId.toHex().data(), qPrintable(fieldName));
	}
	else
	{
		// redis
		if (gCacheItemMap.contains(itemId))
		{
			log_debug("[REDIS] save cache item field %s, %s", itemId.toHex().data(), qPrintable(fieldName));
			redis_store_cache_item_field<QByteArray>(gRedisContext, itemId, fieldName, value);	
		}
		else
		{
			log_debug("[REDIS] not loaded cache item field %s, %s", itemId.toHex().data(), qPrintable(fieldName));
		}
	}
	
	return;
}

void store_cache_item_field(const QByteArray& itemId, const char* fieldName, const qint64& value)
{
	if (gRedisEnable == false)
	{
		// global cache item map
		log_debug("[CACHE] store cache item %s, %s", itemId.toHex().data(), qPrintable(fieldName));
	}
	else
	{
		// redis
		if (gCacheItemMap.contains(itemId))
		{
			log_debug("[REDIS] save cache item field %s, %s", itemId.toHex().data(), qPrintable(fieldName));
			redis_store_cache_item_field<qint64>(gRedisContext, itemId, fieldName, value);	
		}
		else
		{
			log_debug("[REDIS] not loaded cache item field %s, %s", itemId.toHex().data(), qPrintable(fieldName));
		}
	}
	
	return;
}

void store_cache_item_field(const QByteArray& itemId, const char* fieldName, const QMap<QByteArray, ClientInCacheItem>& value)
{
	if (gRedisEnable == false)
	{
		// global cache item map
		log_debug("[CACHE] store cache item %s, %s", itemId.toHex().data(), qPrintable(fieldName));
	}
	else
	{
		// redis
		if (gCacheItemMap.contains(itemId))
		{
			log_debug("[REDIS] save cache item field %s, %s", itemId.toHex().data(), qPrintable(fieldName));
			redis_store_cache_item_field<QMap<QByteArray, ClientInCacheItem>>(gRedisContext, itemId, fieldName, value);	
		}
		else
		{
			log_debug("[REDIS] not loaded cache item field %s, %s", itemId.toHex().data(), qPrintable(fieldName));
		}
	}
	
	return;
}

void save_cache_item(const QByteArray& itemId, const CacheItem& cacheItem)
{
	if (gRedisEnable == false)
	{
		// global cache item map
		gCacheItemMap[itemId] = cacheItem;
	}
	else
	{
		// redis
		redis_save_cache_item(gRedisContext, itemId, cacheItem);
	}
	
	return;
}

void remove_cache_item(const QByteArray& itemId)
{
	if (gRedisEnable == false)
	{
		if (is_cache_item(itemId))
		{
			log_debug("[CACHE] remove cache item %s", itemId.toHex().data());
			gCacheItemMap.remove(itemId);
		}
	}
	else
	{
		// redis
		redis_remove_cache_item(gRedisContext, itemId);
	}
	
	return;
}

QList<QByteArray> get_cache_item_ids()
{
	QList<QByteArray> ret;

	if (gRedisEnable == false)
	{
		ret = gCacheItemMap.keys();
	}
	else
	{
		// redis
		ret = redis_get_cache_item_ids(gRedisContext);
	}

	return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Cache Thread
bool gCacheThreadAllowFlag = true;
static int gMainThreadRunning = 0;
static bool gCacheThreadRunning = false;

void pause_cache_thread()
{
	if (gMainThreadRunning)
	{
		gMainThreadRunning++;
		return;
	}
	
	while (gCacheThreadRunning)
	{
		QThread::usleep(1);
	}
	
	gMainThreadRunning++;
}

void resume_cache_thread()
{
	if (!gMainThreadRunning)
	{
		return;
	}
	
	gMainThreadRunning--;
}

static void remove_old_cache_items()
{
	qint64 currMTime = QDateTime::currentMSecsSinceEpoch();
	qint64 accessTimeoutMSeconds = gAccessTimeoutSeconds * 1000;
	qint64 responseTimeoutMSeconds = gResponseTimeoutSeconds * 1000;

	while (accessTimeoutMSeconds > 0)
	{
		QList<QByteArray> cacheItemIdList = get_cache_item_ids();
		QList<QByteArray> deleteIdList;
		int itemCount = cacheItemIdList.count();

		// Remove items where the value is greater than 30
		for	(int i=0; i < itemCount; i++)
		{
			QByteArray itemId = cacheItemIdList[i];
			CacheItem *pCacheItem = load_cache_item(itemId);
			if (pCacheItem->methodType == CacheMethodType::CACHE_METHOD)
			{
				if (pCacheItem->refreshFlag & AUTO_REFRESH_UNERASE)
				{
					log_debug("[CACHE] detected unerase method(%s) %s", qPrintable(pCacheItem->methodName), itemId.toHex().data());
					continue;
				}
				qint64 accessDiff = currMTime - pCacheItem->lastAccessTime;
				if (accessDiff > accessTimeoutMSeconds)
				{
					// remove cache item
					log_debug("[CACHE] deleting cache item for access timeout %s", itemId.toHex().data());
					deleteIdList.append(itemId);  // Safely erase and move to the next item
					continue;
				} 
			}
			else if (pCacheItem->methodType == CacheMethodType::SUBSCRIBE_METHOD && pCacheItem->cachedFlag == true)
			{
				qint64 refreshDiff = currMTime - pCacheItem->lastRefreshTime;
				
				if (pCacheItem->clientMap.count() == 0 || refreshDiff > responseTimeoutMSeconds)
				{
					log_debug("[WS] checking subscription item clientCount=%d diff=%ld", pCacheItem->clientMap.count(), refreshDiff);

					// add unsubscribe request item for cache thread
					if (pCacheItem->orgMsgId.isEmpty() == false)
					{
						UnsubscribeRequestItem reqItem;
						reqItem.subscriptionStr = pCacheItem->subscriptionStr;
						reqItem.from = pCacheItem->requestPacket.from;
						reqItem.unsubscribeMethodName = gSubscribeMethodMap[pCacheItem->methodName];
						reqItem.cacheClientId = pCacheItem->cacheClientId;
						gUnsubscribeRequestList.append(reqItem);
					}

					// remove subscription item
					log_debug("[WS] deleting1 subscription item originSubscriptionStr=\"%s\", subscriptionStr=\"%s\"", 
						qPrintable(pCacheItem->orgSubscriptionStr), qPrintable(pCacheItem->subscriptionStr));
					deleteIdList.append(itemId);  // Safely erase and move to the next item
					continue;
				}
			}
		}

		for	(int i=0; i < deleteIdList.count(); i++)
		{
			remove_cache_item(deleteIdList[i]);
		}
		int deleteCount = deleteIdList.count();

		int cacheItemCount = itemCount - deleteCount;
		if (cacheItemCount < gCacheItemMaxCount)
		{
			break;
		}

		log_debug("[CACHE] detected MAX cache item count %d", cacheItemCount);
		accessTimeoutMSeconds -= 1000;
	}
}

void cache_thread()
{
	gCacheThreadAllowFlag = true;
	while (gCacheThreadAllowFlag)
	{
		while (gMainThreadRunning)
		{
			gCacheThreadRunning = false;
			QThread::usleep(1);
		}
		gCacheThreadRunning = true;

		remove_old_cache_items();

		gCacheThreadRunning = false;

		QThread::msleep(100);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Utils
bool is_convertible_to_int(const QString &str) {
    bool ok;
    str.toInt(&ok);  // Attempt conversion to int
    return ok;
}

bool is_cache_method(QString methodStr)
{
	if (gCacheMethodList.contains(methodStr, Qt::CaseInsensitive))
	{
		return true;
	}
	else if (gCacheMethodList.contains("*") && 
		!gSubscribeMethodMap.contains(methodStr))
	{
		foreach(QString subKey, gSubscribeMethodMap.keys())
		{
			if (gSubscribeMethodMap[subKey].toLower() == methodStr)
			{
				return false;
			}
		}
		return true;
	}
	return false;
}

bool is_subscribe_method(QString methodStr)
{
	if (gSubscribeMethodMap.contains(methodStr.toLower()))
	{
		return true;
	}
	return false;
}

bool is_never_timeout_method(QString methodStr, QString paramsStr)
{
	if (gNeverTimeoutMethodList.contains(methodStr, Qt::CaseInsensitive))
	{
		if (QString::compare(paramsStr, "[LIST]", Qt::CaseInsensitive) != 0)
		{
			return true;
		}
	}
	return false;
}

int get_cc_index_from_clientId(QByteArray clientId)
{
	for (int i = 0; i < gWsCacheClientList.count(); i++)
	{
		if (gWsCacheClientList[i].clientId == clientId)
		{
			return i;
		}			
	}
	return -1;
}

int get_cc_next_index_from_clientId(QByteArray clientId)
{
	int ccIndex = get_cc_index_from_clientId(clientId);

	ccIndex += 1;
	if (ccIndex >= gWsCacheClientList.count())
		ccIndex = 0;

	for (int i = ccIndex; i < gWsCacheClientList.count(); i++)
	{
		if (gWsCacheClientList[i].initFlag == true)
		{
			return i;
		}			
	}

	for (int i = 0; i < ccIndex; i++)
	{
		if (gWsCacheClientList[i].initFlag == true)
		{
			return i;
		}			
	}

	return -1;
}

int get_cc_index_from_init_request(ZhttpRequestPacket &p)
{
	QByteArray pId = p.ids.first().id;

	// check if recvInit is from Health_Client or Cache_Client
	HttpHeaders requestHeaders = p.headers;
	QByteArray headerKey = QByteArray("Socket-Owner");
	// Check Health_Client
	if (requestHeaders.contains(headerKey))
	{
		QString headerValue = requestHeaders.get(headerKey).data();
		// Define the regular expression to extract the number
		QRegularExpression regex("Cache_Client(\\d+)");
		QRegularExpressionMatch match = regex.match(headerValue);

		if (match.hasMatch()) 
		{
			QString numberStr = match.captured(1);
			int number = numberStr.toInt(); // Convert to integer

			return number;
		}
	}

	return -1;
}

pid_t create_process_for_cacheclient(QString urlPath, int _no)
{
	char socketHeaderStr[64];
	sprintf(socketHeaderStr, "Socket-Owner:Cache_Client%d", _no);

	pid_t processId = fork();
	if (processId == -1)
	{
		// processId == -1 means error occurred
		log_debug("can't fork to start wscat");
		return -1;
	}
	else if (processId == 0) // child process
	{
		char *bin = (char*)"/usr/bin/wscat";
		
		// create wscat
		char * argv_list[] = {
			bin, 
			(char*)"-H", socketHeaderStr, 
			(char*)"-c", (char*)qPrintable(urlPath), 
			NULL
		};
		execve(bin, argv_list, NULL);
		
		//set_debugLogLevel(true);
		log_debug("failed to start wscat error=%d", errno);

		exit(0);
	}

	// parent process
	log_debug("[WS] created new cache client%d parent=%d processId=%d", _no, getpid(), processId);

	return processId;
}

int get_main_cc_index()
{
	for (int i=0; i<gWsCacheClientList.count(); i++)
	{
		if (gWsCacheClientList[i].initFlag == true)
			return i;
	}
	return -1;
}

void parse_json_map(QVariantMap& jsonData, QString keyName, QVariantMap& jsonMap)
{
	for(QVariantMap::const_iterator item = jsonData.begin(); item != jsonData.end(); ++item) 
	{
		QString itemKey = item.key();
		QVariant itemVal = item.value();
		// if has the same key, skip
		if (jsonMap.contains(itemKey))
			continue;

		itemKey = keyName.isNull() ? itemKey : keyName+">>"+itemKey;

		// add exception for id field
		if (itemKey == "id")
		{
			if (itemVal.type() == QVariant::String)
			{
				QString strVal = "\"";
				strVal += itemVal.toString();
				strVal += "\"";
				jsonMap[itemKey] = strVal;
			}
			else if (itemVal.canConvert<QString>())
			{
				jsonMap[itemKey] = itemVal.toString();
			}
		}
		else if (itemVal.canConvert<QString>())
		{
			jsonMap[itemKey] = itemVal.toString();
		}
		else if (itemVal.type() == QVariant::Map)
		{
			QVariantMap mapData = itemVal.toMap();
			parse_json_map(mapData, itemKey, jsonMap);
		}
		else if (itemVal.type() == QVariant::List)
		{
			QString tmpStr = "";
			int i = 0;
			for (QVariant m : itemVal.toList())
			{
				if (m.canConvert<QString>())
				{
					tmpStr += m.toString() + "+";
				}
				else if (m.type() == QVariant::List)
				{
					for (QVariant n : m.toList())
					{
						if (n.canConvert<QString>())
						{
							QString s = n.toString();
							if (s.length() == 0)
							{
								tmpStr += "null";
								tmpStr += "+";
							}
							else
							{
								tmpStr += n.toString() + "+";
							}
						}
						else
						{
							log_debug("[WS] invalid type=%s", n.typeName());
						}
					}
					// remove '+', '/' at the end
					while (tmpStr.endsWith("+") || tmpStr.endsWith("/"))
					{
						tmpStr.remove(tmpStr.length()-1, 1);
					}
					tmpStr += "/";
				}
				else if (m.type() == QVariant::Map)
				{
					QVariantMap mapData = m.toMap();
					parse_json_map(mapData, itemKey+">>"+QString::number(i), jsonMap);
				}
				i++;
			}

			// remove '+', '/' at the end
			while (tmpStr.endsWith("+") || tmpStr.endsWith("/"))
			{
				tmpStr.remove(tmpStr.length()-1, 1);
			}
			
			jsonMap[itemKey] = (tmpStr.length() > 0) ? tmpStr : "[LIST]";
		}
		else
		{
			log_debug("[WS] unknown parse json type=%s", itemVal.typeName());
		}
	}
}

int parse_json_msg(QVariant jsonMsg, QVariantMap& jsonMap)
{
	// parse body as JSON string
	QJsonParseError error;
	QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonMsg.toByteArray(), &error);

	if(error.error != QJsonParseError::NoError)
		return -1;
	
	if(jsonDoc.isObject())
	{
		QVariantMap jsonData = jsonDoc.object().toVariantMap();
		parse_json_map(jsonData, NULL, jsonMap);
	}
	else if(jsonDoc.isArray())
	{
		QVariantList jsonData = jsonDoc.array().toVariantList();
		for(const QVariant& item : jsonData) 
		{
			if (item.type() == QVariant::Map)
			{
				QVariantMap mapData = item.toMap();
				parse_json_map(mapData, NULL, jsonMap);
				break;
			}
		}
	}
	else
	{
		return -1;
	}

	return 0;
}

int parse_packet_msg(Scheme scheme, const ZhttpRequestPacket& packet, PacketMsg& packetMsg)
{
	// Parse json message
	QVariantMap jsonMap;
	if (parse_json_msg(packet.toVariant().toHash().value("body"), jsonMap) < 0)
	{
		log_debug("[JSON] failed to parse json");
		return -1;
	}

	for(QVariantMap::const_iterator item = jsonMap.begin(); item != jsonMap.end(); ++item) 
	{
		log_debug("key = %s, value = %s", qPrintable(item.key()), qPrintable(item.value().toString().mid(0,128)));
	}

	if (jsonMap.contains(gMsgIdAttrName))
	{
		packetMsg.id = jsonMap[gMsgIdAttrName].toString();
		if (jsonMap[gMsgIdAttrName].type() == QVariant::String) 
		{
			packetMsg.id = QString("\"%1\"").arg(packetMsg.id);
		}
	}
	packetMsg.id = jsonMap.contains(gMsgIdAttrName) ? jsonMap[gMsgIdAttrName].toString() : "";
	packetMsg.method = jsonMap.contains(gMsgMethodAttrName) ? jsonMap[gMsgMethodAttrName].toString().toLower() : NULL;
	packetMsg.result = jsonMap.contains(gResultAttrName) ? jsonMap[gResultAttrName].toString() : "";
	packetMsg.isResultNull = false;
	if (jsonMap.contains(gResultAttrName) && packetMsg.result.isEmpty())
		packetMsg.isResultNull = true;
	packetMsg.params = jsonMap.contains(gMsgParamsAttrName) ? jsonMap[gMsgParamsAttrName].toString() : "";
	if (scheme == Scheme::http)
		packetMsg.paramsHash = build_hash_key(jsonMap, "HTTP+");
	else
		packetMsg.paramsHash = build_hash_key(jsonMap, "WS+");
	packetMsg.subscription = jsonMap.contains(gSubscriptionAttrName) ? jsonMap[gSubscriptionAttrName].toString() : "";

	return 0;
}

int parse_packet_msg(Scheme scheme, const ZhttpResponsePacket& packet, PacketMsg& packetMsg)
{
	// Parse json message
	QVariantMap jsonMap;
	if (parse_json_msg(packet.toVariant().toHash().value("body"), jsonMap) < 0)
	{
		log_debug("[JSON] failed to parse json");
		return -1;
	}

	for(QVariantMap::const_iterator item = jsonMap.begin(); item != jsonMap.end(); ++item) 
	{
		log_debug("key = %s, value = %s", qPrintable(item.key()), qPrintable(item.value().toString().mid(0,128)));
	}

	if (jsonMap.contains(gMsgIdAttrName))
	{
		packetMsg.id = jsonMap[gMsgIdAttrName].toString();
		if (jsonMap[gMsgIdAttrName].type() == QVariant::String) 
		{
			packetMsg.id = QString("\"%1\"").arg(packetMsg.id);
		}
	}
	packetMsg.id = jsonMap.contains(gMsgIdAttrName) ? jsonMap[gMsgIdAttrName].toString() : "";
	packetMsg.method = jsonMap.contains(gMsgMethodAttrName) ? jsonMap[gMsgMethodAttrName].toString().toLower() : NULL;
	packetMsg.result = jsonMap.contains(gResultAttrName) ? jsonMap[gResultAttrName].toString() : "";
	packetMsg.isResultNull = false;
	if (jsonMap.contains(gResultAttrName) && packetMsg.result.isEmpty())
		packetMsg.isResultNull = true;
	packetMsg.params = jsonMap.contains(gMsgParamsAttrName) ? jsonMap[gMsgParamsAttrName].toString() : "";
	if (scheme == Scheme::http)
		packetMsg.paramsHash = build_hash_key(jsonMap, "HTTP+");
	else
		packetMsg.paramsHash = build_hash_key(jsonMap, "WS+");
	packetMsg.subscription = jsonMap.contains(gSubscriptionAttrName) ? jsonMap[gSubscriptionAttrName].toString() : "";

	return 0;
}

void replace_id_field(QByteArray &body, QString oldId, int newId)
{
	// new pattern
	char newPattern[64];
	qsnprintf(newPattern, 64, "\"id\":%d", newId);

	// find pattern
	for (int i = 0; i < 20; i++)
	{
		QString iSpace = "";
		QString jSpace = "";
		for (int k = 0; k < i; k++)
		{
			iSpace += " ";
		}
		for (int j = 0; j < 20; j++)
		{
			for (int k = 0; k < j; k++)
			{
				jSpace += " ";
			}
			QString oldPattern = QString("\"id\"") + iSpace + QString(":") + jSpace + oldId;
			int idx = body.indexOf(oldPattern);
			if (idx >= 0)
			{
				body.replace(idx, oldPattern.length(), newPattern);
				return;
			}
		}
	}
}

void replace_id_field(QByteArray &body, QString oldId, QString newId)
{
	// new pattern
	char newPattern[64];
	qsnprintf(newPattern, 64, "\"id\":%s", qPrintable(newId));

	// find pattern
	for (int i = 0; i < 20; i++)
	{
		QString iSpace = "";
		QString jSpace = "";
		for (int k = 0; k < i; k++)
		{
			iSpace += " ";
		}
		for (int j = 0; j < 20; j++)
		{
			for (int k = 0; k < j; k++)
			{
				jSpace += " ";
			}
			QString oldPattern = QString("\"id\"") + iSpace + QString(":") + jSpace + oldId;
			int idx = body.indexOf(oldPattern);
			if (idx >= 0)
			{
				body.replace(idx, oldPattern.length(), newPattern);
				return;
			}
		}
	}
}

void replace_id_field(QByteArray &body, int oldId, QString newId)
{
	// new pattern
	char newPattern[64];
	qsnprintf(newPattern, 64, "\"id\":%s", qPrintable(newId));

	// find pattern
	for (int i = 0; i < 20; i++)
	{
		QString iSpace = "";
		QString jSpace = "";
		for (int k = 0; k < i; k++)
		{
			iSpace += " ";
		}
		for (int j = 0; j < 20; j++)
		{
			for (int k = 0; k < j; k++)
			{
				jSpace += " ";
			}
			QString oldPattern = QString("\"id\"") + iSpace + QString(":") + jSpace + QString::number(oldId);
			int idx = body.indexOf(oldPattern);
			if (idx >= 0)
			{
				body.replace(idx, oldPattern.length(), newPattern);
				return;
			}
		}
	}
}

void replace_result_field(QByteArray &body, QString oldResult, QString newResult)
{
	if (oldResult == newResult)
	{
		return;
	}

	QString oldPattern0 = "\"result\":\"" + oldResult + "\"";
	QString oldPattern1 = "\"result\": \"" + oldResult + "\"";

	char newPattern0[64], newPattern1[64];
	qsnprintf(newPattern0, 64, "\"result\":\"%s\"", qPrintable(newResult));
	qsnprintf(newPattern1, 64, "\"result\": \"%s\"", qPrintable(newResult));

	int idx = body.indexOf(oldPattern0);
	if (idx >= 0)
	{
		body.replace(idx, oldPattern0.length(), newPattern0);
		return;
	}

	idx = body.indexOf(oldPattern1);
	if (idx >= 0)
	{
		body.replace(idx, oldPattern1.length(), newPattern1);
	}
}

void replace_subscription_field(QByteArray &body, QString oldSubscription, QString newSubscription)
{
	if (oldSubscription == newSubscription)
	{
		return;
	}

	QString oldPattern0 = "\"subscription\":\"" + oldSubscription + "\"";
	QString oldPattern1 = "\"subscription\": \"" + oldSubscription + "\"";

	char newPattern0[64], newPattern1[64];
	qsnprintf(newPattern0, 64, "\"subscription\":\"%s\"", qPrintable(newSubscription));
	qsnprintf(newPattern1, 64, "\"subscription\": \"%s\"", qPrintable(newSubscription));

	int idx = body.indexOf(oldPattern0);
	if (idx >= 0)
	{
		body.replace(idx, oldPattern0.length(), newPattern0);
		return;
	}

	idx = body.indexOf(oldPattern1);
	if (idx >= 0)
	{
		body.replace(idx, oldPattern1.length(), newPattern1);
	}
}

QByteArray calculate_response_hash_val(QByteArray &responseBody, int idVal)
{
	QByteArray out = responseBody;
	// replace id str in response
	replace_id_field(out, idVal, 0);

	return QCryptographicHash::hash(out,QCryptographicHash::Sha1);
}

QByteArray calculate_response_seckey_from_init_request(ZhttpRequestPacket &p)
{
	// parse request packet header
	HttpHeaders requestHeaders = p.headers;
	if (requestHeaders.contains("Sec-WebSocket-Key"))
	{
		QByteArray requestKey = requestHeaders.get("Sec-WebSocket-Key");
		QByteArray responseKey = QCryptographicHash::hash((requestKey + MAGIC_STRING), QCryptographicHash::Sha1).toBase64();

		log_debug("[WS] get ws response key for init request requestKey=%s responseKey=%s", requestKey.data(), responseKey.data());

		return responseKey;
	}
	return QByteArray("");
}

QByteArray build_hash_key(QVariantMap &jsonMap, QString startingStr)
{
	QString hashKeyStr = startingStr;
	for (int i = 0; i < gCacheKeyItemList.count(); i++)
	{
		CacheKeyItem keyItem = gCacheKeyItemList[i];
		QString keyVal = "";
		if (keyItem.flag == RAW_VALUE)
		{
			keyVal += keyItem.keyName;
		}
		else
		{
			if (keyItem.flag == JSON_PAIR)
			{
				keyVal += keyItem.keyName + ":";
			}

			for(QVariantMap::const_iterator item = jsonMap.begin(); item != jsonMap.end(); ++item)
			{
				QString iKey = item.key();
				QString iValue = item.value().toString();

				if (!iKey.compare(keyItem.keyName, Qt::CaseInsensitive))
				{
					if (jsonMap[keyItem.keyName].toString().length() > 0)
						keyVal += jsonMap[keyItem.keyName].toString();
					else
						keyVal += " ";
				}
				else if (iKey.indexOf(keyItem.keyName+">>", 0, Qt::CaseInsensitive) == 0)
				{
					keyVal += iKey.toLower() + "->" + iValue;
				}
			}
		}
		if (keyVal.length() > 0)
		{
			hashKeyStr += keyVal;
			if ((i+1) < gCacheKeyItemList.count())
			{
				hashKeyStr += "+";
			}
		}
	}
	log_debug("[HASH] Hash-Key-Str = %s", qPrintable(hashKeyStr.mid(0,128)));

	return QCryptographicHash::hash(hashKeyStr.toUtf8(),QCryptographicHash::Sha1);
}

int check_multi_packets_for_ws_request(ZhttpRequestPacket &p)
{
	QByteArray pId = p.ids.first().id;
	// Check if multi-parts request
	if (gWsMultiPartRequestItemMap.contains(pId))
	{
		// this is middle packet of multi-request
		if (p.more == true)
		{
			log_debug("[WS] Detected middle of multi-parts request");
			gWsMultiPartRequestItemMap[pId].body.append(p.body);

			return -1;
		}
		else // this is end packet of multi-request
		{
			log_debug("[WS] Detected end of multi-parts request");
			gWsMultiPartRequestItemMap[pId].body.append(p.body);
			p.body = gWsMultiPartRequestItemMap[pId].body;

			gWsMultiPartRequestItemMap.remove(pId);
		}
	}
	else
	{
		// this is first packet of multi-request
		if (p.more == true)
		{
			log_debug("[WS] Detected start of multi-parts request");

			// register new multi-request item
			gWsMultiPartRequestItemMap[pId] = p;
			
			return -1;
		}
	}

	return 0;
}

int check_multi_packets_for_ws_response(ZhttpResponsePacket &p)
{
	QByteArray pId = p.ids.first().id;
	// Check if multi-parts response
	if (gWsMultiPartResponseItemMap.contains(pId))
	{
		// this is middle packet of multi-response
		if (p.more == true)
		{
			log_debug("[WS] Detected middle of multi-parts response");
			gWsMultiPartResponseItemMap[pId].body.append(p.body);

			return -1;
		}
		else // this is end packet of multi-response
		{
			log_debug("[WS] Detected end of multi-parts response");
			gWsMultiPartResponseItemMap[pId].body.append(p.body);
			p.body = gWsMultiPartResponseItemMap[pId].body;

			gWsMultiPartResponseItemMap.remove(pId);
		}
	}
	else
	{
		// this is first packet of multi-response
		if (p.more == true)
		{
			log_debug("[WS] Detected start of multi-parts response");

			// register new multi-response item
			gWsMultiPartResponseItemMap[pId] = p;
			
			return -1;
		}
	}

	return 0;
}

int update_request_seq(const QByteArray &clientId)
{
	int ret = -1;

	if (gWsClientMap.contains(clientId)) 
	{
		gWsClientMap[clientId].lastRequestSeq += 1;
		ret = gWsClientMap[clientId].lastRequestSeq;
	}
	else if (gHttpClientMap.contains(clientId)) 
	{
		gHttpClientMap[clientId].lastRequestSeq += 1;
		ret = gHttpClientMap[clientId].lastRequestSeq;
	}
	else // cache client
	{
		int ccIndex = get_cc_index_from_clientId(clientId);
		if (ccIndex >= 0)
		{
			gWsCacheClientList[ccIndex].lastRequestSeq += 1;
			ret = gWsCacheClientList[ccIndex].lastRequestSeq;
		}
	}
	
	return ret;
}

int get_client_new_response_seq(const QByteArray &clientId)
{
	int ret = -1;
	if (gWsClientMap.contains(clientId)) 
	{
		ret = gWsClientMap[clientId].lastResponseSeq + 1;
	}
	else if (gHttpClientMap.contains(clientId)) 
	{
		ret = gHttpClientMap[clientId].lastResponseSeq + 1;
	}
	else // cache client
	{
		int ccIndex = get_cc_index_from_clientId(clientId);
		if (ccIndex >= 0)
			ret = gWsCacheClientList[ccIndex].lastResponseSeq + 1;
	}
	
	return ret;
}

void update_client_response_seq(const QByteArray &clientId, int seqNum)
{
	if (gWsClientMap.contains(clientId)) 
	{
		gWsClientMap[clientId].lastResponseSeq = seqNum;
	}
	else if (gHttpClientMap.contains(clientId)) 
	{
		gHttpClientMap[clientId].lastResponseSeq = seqNum;
	}
	else // cache client
	{
		int ccIndex = get_cc_index_from_clientId(clientId);
		if (ccIndex >= 0)
			gWsCacheClientList[ccIndex].lastResponseSeq = seqNum;
	}
}


void send_http_post_request_with_refresh_header(QString backend, QByteArray postData, char *headerVal)
{
	// Create the QNetworkAccessManager
	QNetworkAccessManager *manager = new QNetworkAccessManager();

	// Set the target URL
	QUrl url(backend);
	QNetworkRequest request(url);

	// Set request headers
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setRawHeader(HTTP_REFRESH_HEADER, headerVal);

	// Send the POST request asynchronously
	QNetworkReply *reply = manager->post(request, postData);
	/*
	// Ignore the response - don't connect any slots to 'reply->finished'
	QObject::connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);

	// Optionally, delete manager after sending request (if you don't need it later)
	QObject::connect(reply, &QNetworkReply::destroyed, manager, &QNetworkAccessManager::deleteLater);
	*/
	// Disconnect immediately without waiting for a reply
	QObject::connect(reply, &QNetworkReply::finished, [reply]() {
		reply->deleteLater();  // Clean up reply object
	});

	// Optionally, delete manager after request is sent
	QObject::connect(reply, &QNetworkReply::finished, manager, &QNetworkAccessManager::deleteLater);
}

int get_next_cache_refresh_interval(const QByteArray &itemId)
{
	int timeInterval = 0;

	CacheItem *pCacheItem = load_cache_item(itemId);
	if (pCacheItem == NULL)
	{
		log_debug("[CACHE] not exist cache item %s", itemId.toHex().data());
		return -1;
	}

	if (pCacheItem->cachedFlag == true)
	{
		// if it`s websocket and cache method
		if (pCacheItem->proto == Scheme::http ||
			(pCacheItem->proto == Scheme::websocket && pCacheItem->methodType == CacheMethodType::CACHE_METHOD))
		{
			if (pCacheItem->refreshFlag & AUTO_REFRESH_NEVER_TIMEOUT)
			{
				timeInterval = 0;
			}
			else if (pCacheItem->refreshFlag & AUTO_REFRESH_SHORTER_TIMEOUT)
			{
				timeInterval = gShorterTimeoutSeconds;
			}
			else if (pCacheItem->refreshFlag & AUTO_REFRESH_LONGER_TIMEOUT)
			{
				timeInterval = gLongerTimeoutSeconds;
			}
			else
			{
				timeInterval = gCacheTimeoutSeconds;
			}
		}
	}
	else
	{
		// set interval to the fixed value
		timeInterval = 5;
	}

	return timeInterval;
}

QString get_switched_http_backend_url(QString currUrl)
{
	int index = -1;

	// Iterate through the list
	for (int i = 0; i < gHttpBackendUrlList.size(); ++i) 
	{
		if (gHttpBackendUrlList.at(i).compare(currUrl, Qt::CaseInsensitive) == 0) 
		{
			index = i;
			break;  // Stop after finding the match
		}
	}

	// Check the result
	if (index != -1) 
	{
		index++;
		if (index >= gHttpBackendUrlList.size())
			index = 0;
	}
	else
	{
		index = 0;
	}

	return gHttpBackendUrlList[index];
}

QString get_switched_ws_backend_url(QString currUrl)
{
	int index = -1;

	// Iterate through the list
	for (int i = 0; i < gWsBackendUrlList.size(); ++i) 
	{
		if (gWsBackendUrlList.at(i).compare(currUrl, Qt::CaseInsensitive) == 0) 
		{
			index = i;
			break;  // Stop after finding the match
		}
	}

	// Check the result
	if (index != -1) 
	{
		index++;
		if (index >= gWsBackendUrlList.size())
			index = 0;
	}
	else
	{
		index = 0;
	}

	return gWsBackendUrlList[index];
}
