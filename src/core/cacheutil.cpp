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
#include <QTimer>
#include <QtConcurrent>

#include "qtcompat.h"
#include "tnetstring.h"
#include "log.h"

extern bool gCacheEnable;
extern QStringList gHttpBackendUrlList;
extern QStringList gWsBackendUrlList;

// Response Body for cache item
QHash<QByteArray, QByteArray> gCacheResponseBuffer;

QHash<QByteArray, CacheItem> gCacheItemMap;

extern QString gMsgIdAttrName;
extern QString gMsgMethodAttrName;
extern QString gMsgParamsAttrName;
extern QString gResultAttrName;
extern QString gSubscriptionAttrName;
extern QString gSubscribeBlockAttrName;
extern QString gSubscribeChangesAttrName;

extern QStringList gCacheMethodList;
extern QHash<QString, QString> gSubscribeMethodMap;
extern QHash<QByteArray, QList<UnsubscribeRequestItem>> gUnsubscribeRequestMap;
extern QList<QByteArray>  gDeleteClientList;
extern QStringList gNeverTimeoutMethodList;
extern QList<CacheKeyItem> gCacheKeyItemList;

// multi packets params
extern QHash<QByteArray, ZhttpResponsePacket> gHttpMultiPartResponseItemMap;
extern QHash<QByteArray, ZhttpRequestPacket> gWsMultiPartRequestItemMap;
extern QHash<QByteArray, ZhttpResponsePacket> gWsMultiPartResponseItemMap;

extern QList<ClientItem> gWsCacheClientList;
extern QHash<QByteArray, ClientItem> gWsClientMap;
extern QHash<QByteArray, ClientItem> gHttpClientMap;

extern int gAccessTimeoutSeconds;
extern int gResponseTimeoutSeconds;
extern int gClientNoRequestTimeoutSeconds;
extern int gCacheTimeoutSeconds;
extern int gShorterTimeoutSeconds;
extern int gLongerTimeoutSeconds;
extern int gCacheItemMaxCount;

// redis
extern bool gRedisEnable;

// count method group
extern QHash<QString, QStringList> gCountMethodGroupMap;

// definitions for cache
#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define REDIS_CACHE_ID_HEADER	"PUSHPIN : "

// prometheus status
extern QList<QString> gCacheMethodRequestCountList;
extern QList<QString> gCacheMethodResponseCountList;
extern quint32 numRequestReceived, numMessageSent, numWsConnect;
extern quint32 numClientCount, numHttpClientCount, numWsClientCount;
extern quint32 numRpcAuthor, numRpcBabe, numRpcBeefy, numRpcChain, numRpcChildState;
extern quint32 numRpcContracts, numRpcDev, numRpcEngine, numRpcEth, numRpcNet;
extern quint32 numRpcWeb3, numRpcGrandpa, numRpcMmr, numRpcOffchain, numRpcPayment;
extern quint32 numRpcRpc, numRpcState, numRpcSyncstate, numRpcSystem, numRpcSubscribe;
extern quint32 numCacheInsert, numCacheHit, numNeverTimeoutCacheInsert, numNeverTimeoutCacheHit;
extern quint32 numCacheLookup, numCacheExpiry, numRequestMultiPart;
extern quint32 numSubscriptionInsert, numSubscriptionHit, numSubscriptionLookup, numSubscriptionExpiry, numResponseMultiPart;
extern quint32 numCacheItem, numAutoRefreshItem, numAREItemCount, numSubscriptionItem, numNeverTimeoutCacheItem;
extern QHash<QString, int> groupMethodCountMap;

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// HiRedis
void redis_removeall_cache_item() 
{
	log_debug("[QQQ] redis_removeall_cache_item");
	auto conn = RedisPool::instance()->acquire();

	if (!conn)
	{
		log_debug("[REDIS] CONN failed\n");
		return;
	}

	redisReply* reply = (redisReply*)redisCommand(conn.data(), "FLUSHDB");

	if (reply->type == REDIS_REPLY_STATUS && std::string(reply->str) == "OK") 
	{
		log_debug("[REDIS] Database cleared successfully.");
	}

	if (reply != nullptr)
		freeReplyObject(reply);
	//pool.release(conn);

	return;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Cache Item
bool is_cache_item(const QByteArray& itemId)
{
	// global cache item map
	return gCacheItemMap.contains(itemId);
}

CacheItem* load_cache_item(const QByteArray& itemId)
{
	CacheItem* ret = NULL;

	// global cache item map
	if (!is_cache_item(itemId))
	{
		log_debug("[CACHE] not found cache item %s", itemId.toHex().data());
		return NULL;
	}
	ret = &gCacheItemMap[itemId];

	return ret;
}

void create_cache_item(const QByteArray& itemId, const CacheItem& cacheItem)
{
	// global cache item map
	gCacheItemMap[itemId] = cacheItem;
	
	// save prometheus
	if (cacheItem.methodType == CACHE_METHOD)
	{
		if ((cacheItem.refreshFlag & AUTO_REFRESH_NEVER_TIMEOUT) != 0)
			numNeverTimeoutCacheInsert++;
		else
			numCacheInsert++;
	}
	else if (cacheItem.methodType == SUBSCRIBE_METHOD)
	{
		numSubscriptionInsert++;
	}
	
	return;
}

void remove_cache_item(const QByteArray& itemId)
{
	// prometheus status
	if (gCacheItemMap[itemId].methodType == CACHE_METHOD)
	{
		numCacheExpiry++;
	}
	else if (gCacheItemMap[itemId].methodType == SUBSCRIBE_METHOD)
	{
		if (gCacheItemMap[itemId].newMsgId != -1)
			numSubscriptionExpiry++;
	}

	if (is_cache_item(itemId))
	{
		log_debug("[CACHE] remove cache item %s", itemId.toHex().data());
		gCacheItemMap.remove(itemId);
	}

	return;
}

QList<QByteArray> get_cache_item_ids()
{
	return gCacheItemMap.keys();
}

void redis_store_cache_response(const QByteArray& itemId, const QByteArray& response) 
{
	auto conn = RedisPool::instance()->acquire();

	if (!conn)
	{
		log_debug("[REDIS] CONN failed\n");
		return;
	}

	QByteArray key = REDIS_CACHE_ID_HEADER + itemId;

	redisReply* reply = (redisReply*)redisCommand(conn.data(),
		"SET %s %b",
		key.constData(),
		response.constData(), response.size()
	);
	
	if (reply != nullptr) 
		freeReplyObject(reply);
}

QByteArray redis_load_cache_response(const QByteArray& itemId) 
{
	auto conn = RedisPool::instance()->acquire();

	if (!conn)
	{
		log_debug("[REDIS] CONN failed\n");
		return QByteArray();
	}

	QByteArray key = REDIS_CACHE_ID_HEADER + itemId;

	redisReply* reply = (redisReply*)redisCommand(conn.data(),
		"GET %s",
		key.constData()
	);

	if (reply == nullptr)
		return QByteArray();

	QByteArray response(reply->str, reply->len);
	
	if (reply != nullptr)
		freeReplyObject(reply);

	return response;
}

void store_cache_response_buffer(const QByteArray& itemId, const QByteArray& responseBuf, QString msgId)
{
	QByteArray buff = responseBuf;

	// remove connmgr Txxx:
	QByteArray prefix = " T";
	int start = buff.indexOf(prefix);
	if (start != -1) 
	{
		int colon = buff.indexOf(':', start + prefix.length());
		if (colon != -1) 
		{
			buff.remove(0, colon + 1);  // Remove up to and including colon
		}
	}

	// replace id
	prefix = "2:id,";
	start = buff.indexOf(prefix);
	if (start != -1) 
	{
		int end = buff.indexOf(',', start + prefix.length());
		if (end != -1) 
		{
			buff.replace(start, end - start, "2:id,__ID__");  // Replace
		}
	}

	// replace seq
	prefix = "3:seq,";
	start = buff.indexOf(prefix);
	if (start != -1) 
	{
		int end = buff.indexOf('#', start + prefix.length());
		if (end != -1) 
		{
			buff.replace(start, end - start, "3:seq,__SEQ__");  // Replace
		}
	}

	// replace from
	prefix = "4:from,";
	start = buff.indexOf(prefix);
	if (start != -1) 
	{
		int end = buff.indexOf(',', start + prefix.length());
		if (end != -1) 
		{
			buff.replace(start, end - start, "4:from,__FROM__");  // Replace
		}
	}

	// replace body length
	int msgIdLen = !msgId.isEmpty() ? msgId.length() : 0;
	int bodyLen = 0;
	prefix = "4:body,";
	start = buff.indexOf(prefix);
	if (start != -1) 
	{
		int end = buff.indexOf(':', start + prefix.length());
		QByteArray part = buff.mid(start + prefix.length(), end-start-prefix.length());
		bodyLen = part.toInt();
		QByteArray newPattern = QByteArray("4:body,__BODY__") + QString::number(bodyLen-msgIdLen);
		if (end != -1) 
		{
			buff.replace(start, end - start, newPattern);  // Replace
		}
	}

	if (!msgId.isEmpty())
	{
		// replace msgId
		QByteArray oldPattern = QByteArray("\"id\":") + msgId.toUtf8();
		QByteArray newPattern = QByteArray("\"id\":__MSGID__");
		buff.replace(oldPattern, newPattern);

		// replace Content-Length header
		int bodyLenNumLength = QString::number(bodyLen).length();
		oldPattern = QByteArray("14:Content-Length,") + QByteArray::number(bodyLenNumLength) + QByteArray(":") + QByteArray::number(bodyLen);
		newPattern = QByteArray("14:Content-Length,__CONTENT_LENGTH__");
		buff.replace(oldPattern, newPattern);
	}

	log_debug("[STORE_BUFF] %s", buff.constData());

	if (gRedisEnable == false)
	{
		gCacheResponseBuffer[itemId] = buff;
	}
	else
	{
		redis_store_cache_response(itemId, buff);
	}
}

QByteArray load_cache_response_buffer(const QByteArray& instanceAddress, const QByteArray& itemId, QByteArray packetId, int seqNum, QString msgId, QByteArray from)
{
	QByteArray buff = "";
	if (gRedisEnable == false)
	{
		buff = gCacheResponseBuffer[itemId];
	}
	else
	{
		buff = redis_load_cache_response(itemId);
	}

	// replace id
	int idLen = packetId.length();
	QByteArray oldPattern = QByteArray("2:id,") + QByteArray("__ID__");
	QByteArray newPattern = QByteArray("2:id,") + QByteArray::number(idLen) + QByteArray(":") + packetId;
	buff.replace(oldPattern, newPattern);

	// replace seq
	int seqNumLength = QString::number(seqNum).length();
	oldPattern = QByteArray("3:seq,") + QByteArray("__SEQ__");
	newPattern = QByteArray("3:seq,") + QByteArray::number(seqNumLength) + QByteArray(":") + QByteArray::number(seqNum);
	buff.replace(oldPattern, newPattern);

	// replace from
	int fromLen = from.length();
	oldPattern = QByteArray("4:from,") + QByteArray("__FROM__");
	newPattern = QByteArray("4:from,") + QByteArray::number(fromLen) + QByteArray(":") + from;
	buff.replace(oldPattern, newPattern);

	// replace bodyLen
	int msgIdLen = msgId.length();
	int newLen = 0;
	int startIndex = buff.indexOf("4:body,__BODY__");
	if (startIndex >= 0)
	{
		startIndex += 15;
		int endIndex = buff.indexOf(':', startIndex);
		QByteArray part = buff.mid(startIndex, endIndex-startIndex);
		int orgLen = part.toInt();
		newLen = orgLen + msgIdLen;
		newPattern = QByteArray("4:body,") + QByteArray::number(newLen);
		buff.replace(startIndex-15, endIndex-startIndex+15, newPattern);
	}

	// replace msgId/bodyLen
	startIndex = buff.indexOf("\"id\":__MSGID__");
	if (startIndex >= 0)
	{
		// replace msgId
		newPattern = QByteArray("\"id\":") + msgId.toUtf8();
		buff.replace(startIndex-14, endIndex-startIndex+14, newPattern);

		// replace Content-Length header
		oldPattern = QByteArray("14:Content-Length,__CONTENT_LENGTH__");
		int bodyLenNumLength = QString::number(newLen).length();
		newPattern = QByteArray("14:Content-Length,") + QByteArray::number(bodyLenNumLength) + QByteArray(":") + QByteArray::number(newLen);
		buff.replace(oldPattern, newPattern);
	}

	// add connmgr Txxx:
	int buffLen = buff.length();
	buff = instanceAddress + " T" + QByteArray::number(buffLen-1) + QByteArray(":") + buff;

	log_debug("[LOAD_BUFF] %s", buff.constData());

	return buff;
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
		QList<QByteArray> cacheItemIdList = gCacheItemMap.keys();//get_cache_item_ids();
		QList<QByteArray> deleteIdList;
		int itemCount = cacheItemIdList.count();

		int cacheItemCount = 0;
		int subscribeItemCount = 0;
		int neverTimeoutCacheItemCount = 0;
		int autoRefreshItemCount = 0;

		// Remove items where the value is greater than 30
		for	(int i=0; i < itemCount; i++)
		{
			QByteArray itemId = cacheItemIdList[i];
			CacheItem *pCacheItem = &gCacheItemMap[itemId];//load_cache_item(itemId);
			// prometheus status
			if (pCacheItem->methodType == CACHE_METHOD)
			{
				cacheItemCount++;
				if ((pCacheItem->refreshFlag & AUTO_REFRESH_NEVER_TIMEOUT) != 0)
					neverTimeoutCacheItemCount++;
				else
					autoRefreshItemCount++;
			}
			else if (pCacheItem->methodType == SUBSCRIBE_METHOD)
			{
				subscribeItemCount++;
			}

			if (pCacheItem->methodType == CacheMethodType::CACHE_METHOD)
			{
				if (pCacheItem->refreshFlag & AUTO_REFRESH_UNERASE || pCacheItem->refreshFlag & AUTO_REFRESH_NEVER_TIMEOUT)
				{
					//log_debug("[CACHE] detected unerase method(%s) %s", qPrintable(pCacheItem->methodName), itemId.toHex().data());
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
			else if (pCacheItem->methodType == CacheMethodType::SUBSCRIBE_METHOD)
			{
				qint64 refreshDiff = currMTime - pCacheItem->lastRefreshTime;
				
				if (pCacheItem->clientMap.count() == 0 || refreshDiff > responseTimeoutMSeconds)
				{
					log_debug("[WS] checking subscription item clientCount=%d diff=%ld", pCacheItem->clientMap.count(), refreshDiff);

					// add unsubscribe request item for cache thread
					if (pCacheItem->cachedFlag == true)
					{
						int ccIndex = get_cc_index_from_clientId(pCacheItem->cacheClientId);
						if (ccIndex >= 0)
						{
							QByteArray instanceId = gWsCacheClientList[ccIndex].instanceId;
							UnsubscribeRequestItem reqItem;
							reqItem.subscriptionStr = pCacheItem->subscriptionStr;
							reqItem.from = pCacheItem->requestPacket.from;
							reqItem.unsubscribeMethodName = gSubscribeMethodMap[pCacheItem->methodName];
							reqItem.cacheClientId = pCacheItem->cacheClientId;
							gUnsubscribeRequestMap[instanceId].append(reqItem);
						}
					}

					// remove subscription item
					log_debug("[WS] deleting1 subscription item subscriptionStr=\"%s\"", qPrintable(pCacheItem->subscriptionStr));
					deleteIdList.append(itemId);  // Safely erase and move to the next item
					continue;
				}

				if (pCacheItem->updatedFlag == true)
				{
					pCacheItem->updatedFlag = false;
					store_cache_response_buffer(pCacheItem->subscriptionStr.toUtf8(), pCacheItem->updatedSubscription, QString(""));
				}
			}
		}

		numCacheItem = cacheItemCount;
		numSubscriptionItem = subscribeItemCount;
		numNeverTimeoutCacheItem = neverTimeoutCacheItemCount;
		numAutoRefreshItem = autoRefreshItemCount;
		numAREItemCount = cacheItemCount - autoRefreshItemCount;

		for	(int i=0; i < deleteIdList.count(); i++)
		{
			remove_cache_item(deleteIdList[i]);
		}
		int deleteCount = deleteIdList.count();

		int totalItemCount = itemCount - deleteCount;
		if (totalItemCount < gCacheItemMaxCount)
		{
			break;
		}

		log_debug("[CACHE] detected MAX cache item count %d", totalItemCount);
		accessTimeoutMSeconds -= 1000;
	}
}

void check_old_clients()
{
	qint64 clientNoRequestTimeoutSeconds = gClientNoRequestTimeoutSeconds * 1000;
	qint64 currMTime = QDateTime::currentMSecsSinceEpoch();

	// lookup clients to delete
	foreach(QByteArray id, gHttpClientMap.keys())
	{
		qint64 diffMSeconds = currMTime - gHttpClientMap[id].lastRequestTime;
		if (!gDeleteClientList.contains(id) && (diffMSeconds > clientNoRequestTimeoutSeconds))
		{
			// delete this client
			log_debug("[HTTP] add delete client id=%s", id.data());
			gDeleteClientList.append(id);
		}
	}

	foreach(QByteArray id, gWsClientMap.keys())
	{
		qint64 diffMSeconds = currMTime - gWsClientMap[id].lastRequestTime;
		if (!gDeleteClientList.contains(id) && (diffMSeconds > clientNoRequestTimeoutSeconds))
		{
			// delete this client
			log_debug("[WS] add delete client id=%s", id.data());
			gDeleteClientList.append(id);
			continue;
		}
	}

	// count clients
	numHttpClientCount = gHttpClientMap.count();
	numWsClientCount = gWsClientMap.count();
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
		check_old_clients();

		gCacheThreadRunning = false;

		count_methods();

		QThread::msleep(1000);
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
		!gSubscribeMethodMap.contains(methodStr.toLower()))
	{
		foreach(QString subKey, gSubscribeMethodMap.keys())
		{
			if (gSubscribeMethodMap[subKey].toLower() == methodStr.toLower())
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

int get_cc_next_index_from_clientId(QByteArray clientId, QByteArray instanceId)
{
	int ccIndex = get_cc_index_from_clientId(clientId);

	ccIndex += 1;
	if (ccIndex >= gWsCacheClientList.count())
		ccIndex = 0;

	for (int i = ccIndex; i < gWsCacheClientList.count(); i++)
	{
		if (gWsCacheClientList[i].initFlag == true && gWsCacheClientList[i].instanceId == instanceId)
		{
			return i;
		}			
	}

	for (int i = 0; i < ccIndex; i++)
	{
		if (gWsCacheClientList[i].initFlag == true && gWsCacheClientList[i].instanceId == instanceId)
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

void check_cache_clients()
{
	qint64 currMTime = QDateTime::currentMSecsSinceEpoch();
	for (int i = 0; i < gWsCacheClientList.count(); i++)
	{
		qint64 diff = currMTime - gWsCacheClientList[i].lastResponseTime;
		if (diff > gResponseTimeoutSeconds*1000)
		{
			log_debug("[WS] detected cache client %d response timeout %s", i, gWsCacheClientList[i].clientId.data());
			gWsCacheClientList[i].initFlag = false;
		}

		if (gWsCacheClientList[i].initFlag == false)
		{
			// Remove all items where value with client id
			foreach(QByteArray itemId, gCacheItemMap.keys())
			{
				if (gCacheItemMap[itemId].methodType == CacheMethodType::SUBSCRIBE_METHOD && 
					gCacheItemMap[itemId].cacheClientId == gWsCacheClientList[i].clientId) 
				{
					log_debug("[WS] Remove subscription cache item %s", gCacheItemMap[itemId].subscriptionStr);
					remove_cache_item(itemId);
				}
			}

			log_debug("[WS] killing cache client %d process %d, %s", i, gWsCacheClientList[i].processId, gWsCacheClientList[i].clientId.data());
			kill(gWsCacheClientList[i].processId, SIGTERM);
			gWsCacheClientList[i].processId = create_process_for_cacheclient(gWsCacheClientList[i].urlPath, i);
			gWsCacheClientList[i].lastResponseTime = QDateTime::currentMSecsSinceEpoch();
		}			
	}

	QTimer::singleShot(30 * 1000, [=]() {
		check_cache_clients();
	});
}

int get_main_cc_index(QByteArray instanceId)
{
	for (int i=0; i<gWsCacheClientList.count(); i++)
	{
		if (gWsCacheClientList[i].initFlag == true && gWsCacheClientList[i].instanceId == instanceId)
			return i;
	}
	return -1;
}

static void parse_json_map(QVariantMap& jsonData, QString keyName, QVariantMap& jsonMap)
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

int parse_packet_msg(Scheme scheme, const ZhttpRequestPacket& packet, PacketMsg& packetMsg, const QByteArray& instanceId)
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
	{
		QString subKey = QString("HTTP+");
		packetMsg.paramsHash = build_hash_key(jsonMap, subKey);
	}
	else
	{
		QString subKey = QString("WS+");
		packetMsg.paramsHash = build_hash_key(jsonMap, subKey);
	}
	packetMsg.subscription = jsonMap.contains(gSubscriptionAttrName) ? jsonMap[gSubscriptionAttrName].toString() : "";
	packetMsg.resultBlock = jsonMap.contains(gSubscribeBlockAttrName) ? jsonMap[gSubscribeBlockAttrName].toString() : "";
	packetMsg.resultChanges = jsonMap.contains(gSubscribeChangesAttrName) ? jsonMap[gSubscribeChangesAttrName].toString() : "";

	return 0;
}

int parse_packet_msg(Scheme scheme, const ZhttpResponsePacket& packet, PacketMsg& packetMsg, const QByteArray& instanceId)
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
	{
		QString subKey = QString("HTTP+");
		packetMsg.paramsHash = build_hash_key(jsonMap, subKey);
	}
	else
	{
		QString subKey = QString("WS+");
		packetMsg.paramsHash = build_hash_key(jsonMap, subKey);
	}
	packetMsg.subscription = jsonMap.contains(gSubscriptionAttrName) ? jsonMap[gSubscriptionAttrName].toString() : "";
	packetMsg.resultBlock = jsonMap.contains(gSubscribeBlockAttrName) ? jsonMap[gSubscribeBlockAttrName].toString() : "";
	packetMsg.resultChanges = jsonMap.contains(gSubscribeChangesAttrName) ? jsonMap[gSubscribeChangesAttrName].toString() : "";

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

			// prometheus status
			numRequestMultiPart++;
			
			return -1;
		}
	}

	return 0;
}

int check_multi_packets_for_http_response(ZhttpResponsePacket &p)
{
	QByteArray pId = p.ids.first().id;
	// Check if multi-parts response
	if (gHttpMultiPartResponseItemMap.contains(pId))
	{
		// this is middle packet of multi-response
		if (p.more == true)
		{
			log_debug("[HTTP] Detected middle of multi-parts response");
			gHttpMultiPartResponseItemMap[pId].body.append(p.body);

			return -1;
		}
		else // this is end packet of multi-response
		{
			log_debug("[HTTP] Detected end of multi-parts response");
			gHttpMultiPartResponseItemMap[pId].body.append(p.body);
			p.body = gHttpMultiPartResponseItemMap[pId].body;

			gHttpMultiPartResponseItemMap.remove(pId);

			return 1;
		}
	}
	else
	{
		// this is first packet of multi-response
		if (p.more == true)
		{
			log_debug("[HTTP] Detected start of multi-parts response");

			// register new multi-response item
			gHttpMultiPartResponseItemMap[pId] = p;

			// prometheus status
			numResponseMultiPart++;
			
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
			return 1;
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

			// prometheus status
			numResponseMultiPart++;
			
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
		gWsClientMap[clientId].lastResponseSeq = ret;
	}
	else if (gHttpClientMap.contains(clientId)) 
	{
		ret = gHttpClientMap[clientId].lastResponseSeq + 1;
		gHttpClientMap[clientId].lastResponseSeq = ret;
	}
	else // cache client
	{
		int ccIndex = get_cc_index_from_clientId(clientId);
		if (ccIndex >= 0)
		{
			ret = gWsCacheClientList[ccIndex].lastResponseSeq + 1;
			gWsCacheClientList[ccIndex].lastResponseSeq = ret;
		}
	}
	
	return ret;
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

void count_methods()
{
	// request count
	if (gCacheMethodRequestCountList.count() > 0)
	{
		QString methodName = gCacheMethodRequestCountList[0];
		gCacheMethodRequestCountList.removeAt(0);

		// count methods
		numRequestReceived++;
		if (methodName.indexOf("author_", 0, Qt::CaseInsensitive) == 0) {
			numRpcAuthor++;
			if (methodName.indexOf("submitandwatchextrinsic", 0, Qt::CaseInsensitive) == 7)
				numRpcSubscribe++;
		} else if (methodName.indexOf("babe_", 0, Qt::CaseInsensitive) == 0) {
			numRpcBabe++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 5)
				numRpcSubscribe++;
		} else if (methodName.indexOf("beefy_", 0, Qt::CaseInsensitive) == 0) {
			numRpcBeefy++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 6)
				numRpcSubscribe++;
		} else if (methodName.indexOf("chain_", 0, Qt::CaseInsensitive) == 0) {
			numRpcChain++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 6)
				numRpcSubscribe++;
		} else if (methodName.indexOf("childstate_", 0, Qt::CaseInsensitive) == 0) {
			numRpcChildState++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 11)
				numRpcSubscribe++;
		} else if (methodName.indexOf("contracts_", 0, Qt::CaseInsensitive) == 0) {
			numRpcContracts++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 10)
				numRpcSubscribe++;
		} else if (methodName.indexOf("dev_", 0, Qt::CaseInsensitive) == 0) {
			numRpcDev++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 4)
				numRpcSubscribe++;
		} else if (methodName.indexOf("engine_", 0, Qt::CaseInsensitive) == 0) {
			numRpcEngine++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 7)
				numRpcSubscribe++;
		} else if (methodName.indexOf("eth_", 0, Qt::CaseInsensitive) == 0) {
			numRpcEth++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 4)
				numRpcSubscribe++;
		} else if (methodName.indexOf("net_", 0, Qt::CaseInsensitive) == 0) {
			numRpcNet++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 4)
				numRpcSubscribe++;
		} else if (methodName.indexOf("web3_", 0, Qt::CaseInsensitive) == 0) {
			numRpcWeb3++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 5)
				numRpcSubscribe++;
		} else if (methodName.indexOf("grandpa_", 0, Qt::CaseInsensitive) == 0) {
			numRpcGrandpa++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 8)
				numRpcSubscribe++;
		} else if (methodName.indexOf("mmr_", 0, Qt::CaseInsensitive) == 0) {
			numRpcMmr++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 4)
				numRpcSubscribe++;
		} else if (methodName.indexOf("offchain_", 0, Qt::CaseInsensitive) == 0) {
			numRpcOffchain++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 9)
				numRpcSubscribe++;
		} else if (methodName.indexOf("payment_", 0, Qt::CaseInsensitive) == 0) {
			numRpcPayment++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 8)
				numRpcSubscribe++;
		} else if (methodName.indexOf("rpc_", 0, Qt::CaseInsensitive) == 0) {
			numRpcRpc++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 4)
				numRpcSubscribe++;
		} else if (methodName.indexOf("state_", 0, Qt::CaseInsensitive) == 0) {
			numRpcState++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 6)
				numRpcSubscribe++;
		} else if (methodName.indexOf("sync_state_", 0, Qt::CaseInsensitive) == 0) {
			numRpcSyncstate++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 11)
				numRpcSubscribe++;
		} else if (methodName.indexOf("system_", 0, Qt::CaseInsensitive) == 0) {
			numRpcSystem++;
			if (methodName.indexOf("subscribe", 0, Qt::CaseInsensitive) == 7)
				numRpcSubscribe++;
		}

		// add ws Cache lookup count
		if (is_cache_method(methodName))
		{
			numCacheLookup++;
		}
		else if (is_subscribe_method(methodName))
		{
			numSubscriptionLookup++;
		}

		// user-defined method group count
		foreach(QString groupKey, gCountMethodGroupMap.keys())
		{
			QStringList groupStrList = gCountMethodGroupMap[groupKey];

			if (groupStrList.contains(methodName, Qt::CaseInsensitive))
			{
				groupMethodCountMap[groupKey]++;
			}
		}
	}

	// response count
	if (gCacheMethodResponseCountList.count() > 0)
	{
		QString methodName = gCacheMethodResponseCountList[0];
		gCacheMethodResponseCountList.removeAt(0);

		// count methods
		if (methodName == "HTTP" || methodName == "WS")
			numMessageSent++;
		else if (methodName == "WS_INIT")
			numWsConnect++;
	}

	// client count
	numHttpClientCount = gHttpClientMap.count();
	numWsClientCount = gWsClientMap.count();
	numClientCount = numHttpClientCount + numWsClientCount;
}

void update_prometheus_hit_count(const CacheItem &cacheItem)
{
	// prometheus status
	if (cacheItem.methodType == CACHE_METHOD)
	{
		if ((cacheItem.refreshFlag & AUTO_REFRESH_NEVER_TIMEOUT) != 0)
			numNeverTimeoutCacheHit++;
		else
			numCacheHit++;
	}
	else if (cacheItem.methodType == SUBSCRIBE_METHOD)
	{
		numSubscriptionHit++;
	}
}
