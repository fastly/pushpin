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
#include <hiredis.h>

#include "qtcompat.h"
#include "tnetstring.h"
#include "log.h"

extern bool gCacheEnable;
extern QStringList gHttpBackendUrlList;
extern QStringList gWsBackendUrlList;

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

extern QMap<QByteArray, CacheItem> gCacheItemMap;

extern int gAccessTimeoutSeconds;
extern int gResponseTimeoutSeconds;
extern int gCacheTimeoutSeconds;
extern int gShorterTimeoutSeconds;
extern int gLongerTimeoutSeconds;
extern int gCacheItemMaxCount;

// definitions for cache
#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

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
			//std::cerr << "Connection error: " << c->errstr << std::endl;
			log_debug("Connection error: %s", c->errstr);
			redisFree(c);
		} 
		else 
		{
			//std::cerr << "Connection error: can't allocate redis context" << std::endl;
			log_debug("Connection error: can't allocate redis context");
		}
		return nullptr;
	}
	return c;
}

void storeClientItem(redisContext* context, const ClientItem& item) 
{
	QByteArray key = "client:" + item.clientId;

	redisReply* reply = (redisReply*)redisCommand(context,
		"HSET %b "
		"urlPath %b "
		"processId %d "
		"initFlag %d "
		"resultStr %b "
		"msgIdCount %d "
		"lastRequestSeq %d "
		"lastResponseSeq %d "
		"lastRequestTime %lld "
		"lastResponseTime %lld "
		"receiver %b "
		"from %b "
		"clientId %b",

		key.constData(), key.size(),

		item.urlPath.toUtf8().constData(), item.urlPath.toUtf8().size(),
		item.processId,
		item.initFlag ? 1 : 0,
		item.resultStr.toUtf8().constData(), item.resultStr.toUtf8().size(),
		item.msgIdCount,
		item.lastRequestSeq,
		item.lastResponseSeq,
		static_cast<long long>(item.lastRequestTime),
		static_cast<long long>(item.lastResponseTime),
		item.receiver.constData(), item.receiver.size(),
		item.from.constData(), item.from.size(),
		item.clientId.constData(), item.clientId.size()
	);

	if (reply) freeReplyObject(reply);
}

template <typename T>
void updateClientItemField(redisContext* context, const QByteArray& clientId, const char *fieldName, const T& value) 
{
	QByteArray key = "client:" + clientId;

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

	if (reply != nullptr) 
		freeReplyObject(reply);
}

QByteArray loadClientItemField(redisContext* context, const QByteArray& clientId, const char *fieldName) 
{
	QByteArray key = "client:" + clientId;

	redisReply* reply = (redisReply*)redisCommand(context,
		"HGET %b %s",
		key.constData(), key.size(),
		(char *)fieldName
	);

	if (reply == nullptr)
		return NULL;
	log_debug("%d %s", reply->type, reply->str);
	
	QByteArray ret(reply->str, reply->len);

	freeReplyObject(reply);
	return ret;
}

ClientItem loadClientItem(redisContext* context, const QByteArray& clientId) 
{
	ClientItem item;
	item.clientId = clientId;
	QByteArray key = "client:" + clientId;

	redisReply* reply = (redisReply*)redisCommand(context,
		"HGETALL %b", key.constData(), key.size());

	if (!reply || reply->type != REDIS_REPLY_ARRAY) {
		if (reply) freeReplyObject(reply);
		return item;
	}

	for (size_t i = 0; i < reply->elements; i += 2) {
		QByteArray field(reply->element[i]->str, reply->element[i]->len);
		QByteArray value(reply->element[i + 1]->str, reply->element[i + 1]->len);

		if (field == "urlPath") item.urlPath = QString::fromUtf8(value);
		else if (field == "processId") item.processId = value.toInt();
		else if (field == "initFlag") item.initFlag = (value == "1");
		else if (field == "resultStr") item.resultStr = QString::fromUtf8(value);
		else if (field == "msgIdCount") item.msgIdCount = value.toInt();
		else if (field == "lastRequestSeq") item.lastRequestSeq = value.toInt();
		else if (field == "lastResponseSeq") item.lastResponseSeq = value.toInt();
		else if (field == "lastRequestTime") item.lastRequestTime = value.toLongLong();
		else if (field == "lastResponseTime") item.lastResponseTime = value.toLongLong();
		else if (field == "receiver") item.receiver = value;
		else if (field == "from") item.from = value;
		else if (field == "clientId") item.clientId = value;
	}

	freeReplyObject(reply);
	return item;
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
	QByteArray key = "myKey";
	QByteArray value = "myValue";

	setQByteArrayToRedis(c, key, value);
	QByteArray retrievedValue = getQByteArrayFromRedis(c, key);
	*/

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
	item.from = QByteArray("device42");

	storeClientItem(c, item);

	QByteArray ret = loadClientItemField(c, item.clientId, "urlPath");
	QString urlPath = QString::fromUtf8(ret);
	log_debug("urlPath = %s", qPrintable(urlPath));

	updateClientItemField<QString>(c, item.clientId, "urlPath", "/do/update");
	updateClientItemField<pid_t>(c, item.clientId, "processId", getpid());
	updateClientItemField<bool>(c, item.clientId, "initFlag", true);
	updateClientItemField<QString>(c, item.clientId, "resultStr", "okk");
	updateClientItemField<int>(c, item.clientId, "msgIdCount", 42);
	updateClientItemField<int>(c, item.clientId, "lastRequestSeq", 5);
	updateClientItemField<int>(c, item.clientId, "lastResponseSeq", 5);
	updateClientItemField<time_t>(c, item.clientId, "lastRequestTime", time(nullptr));
	updateClientItemField<time_t>(c, item.clientId, "lastResponseTime", time(nullptr));
	updateClientItemField<QByteArray>(c, item.clientId, "receiver", QByteArray::fromHex("deadbeef"));
	updateClientItemField<QByteArray>(c, item.clientId, "from", QByteArray("device42"));

	ClientItem loaded = loadClientItem(c, item.clientId);
	log_debug("Loaded URL:%s", qPrintable(loaded.urlPath));

	redisFree(c);
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
		// Remove items where the value is greater than 30
		for (auto it = gCacheItemMap.begin(); it != gCacheItemMap.end();) 
		{
			QByteArray itemKey = it.key();
			CacheItem cacheItem = it.value();
			if (cacheItem.methodType == CacheMethodType::CACHE_METHOD)
			{
				if (cacheItem.refreshFlag & AUTO_REFRESH_UNERASE)
				{
					log_debug("[CACHE] detected unerase method(%s) %s", qPrintable(cacheItem.methodName), itemKey.toHex().data());
					++it;  // Move to the next item
					continue;
				}
				qint64 accessDiff = currMTime - cacheItem.lastAccessTime;
				if (accessDiff > accessTimeoutMSeconds)
				{
					// remove cache item
					log_debug("[CACHE] deleting cache item for access timeout %s", itemKey.toHex().data());
					it = gCacheItemMap.erase(it);  // Safely erase and move to the next item
					continue;
				} 
			}
			else if (cacheItem.methodType == CacheMethodType::SUBSCRIBE_METHOD && cacheItem.cachedFlag == true)
			{
				qint64 refreshDiff = currMTime - cacheItem.lastRefreshTime;
				
				if (cacheItem.clientMap.count() == 0 || refreshDiff > responseTimeoutMSeconds)
				{
					log_debug("[WS] checking subscription item clientCount=%d diff=%ld", cacheItem.clientMap.count(), refreshDiff);

					// add unsubscribe request item for cache thread
					if (cacheItem.orgMsgId.isEmpty() == false)
					{
						UnsubscribeRequestItem reqItem;
						reqItem.subscriptionStr = cacheItem.subscriptionStr;
						reqItem.from = cacheItem.requestPacket.from;
						reqItem.unsubscribeMethodName = gSubscribeMethodMap[cacheItem.methodName];
						reqItem.cacheClientId = cacheItem.cacheClientId;
						gUnsubscribeRequestList.append(reqItem);
					}

					// remove subscription item
					log_debug("[WS] deleting1 subscription item originSubscriptionStr=\"%s\", subscriptionStr=\"%s\"", 
						qPrintable(cacheItem.orgSubscriptionStr), qPrintable(cacheItem.subscriptionStr));
					it = gCacheItemMap.erase(it);  // Safely erase and move to the next item
					continue;
				}
			}

			++it;  // Move to the next item
		}

		int cacheItemCount = gCacheItemMap.count();
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

		testRedis();

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

	if (gCacheItemMap[itemId].cachedFlag == true)
	{
		// if it`s websocket and cache method
		if (gCacheItemMap[itemId].proto == Scheme::http ||
			(gCacheItemMap[itemId].proto == Scheme::websocket && gCacheItemMap[itemId].methodType == CacheMethodType::CACHE_METHOD))
		{
			if (gCacheItemMap[itemId].refreshFlag & AUTO_REFRESH_NEVER_TIMEOUT)
			{
				timeInterval = 0;
			}
			else if (gCacheItemMap[itemId].refreshFlag & AUTO_REFRESH_SHORTER_TIMEOUT)
			{
				timeInterval = gShorterTimeoutSeconds;
			}
			else if (gCacheItemMap[itemId].refreshFlag & AUTO_REFRESH_LONGER_TIMEOUT)
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
