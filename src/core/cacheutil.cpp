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

#include "qtcompat.h"
#include "tnetstring.h"
#include "log.h"

// definitions for cache
#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

bool is_cacheclient_inited(QList<CacheClientItem> &cacheClientList)
{
	for (int i = 0; i < cacheClientList.count(); i++)
	{
		if (gWsCacheClientList[i].initFlag == true)
		{
			return true;
		}			
	}
	return false;
}

int get_cacheclient_no_from_response(ZhttpResponsePacket &p, QList<CacheClientItem> &cacheClientList)
{
	QByteArray packetId = gWsInitResponsePacket.ids[0].id;
	for (int i = 0; i < cacheClientList.count(); i++)
	{
		if (gWsCacheClientList[i].clientId == packetId)
		{
			return i;
		}			
	}
	return -1;
}

int get_cacheclient_no_from_init_request(ZhttpRequestPacket &p)
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

pid_t create_process_for_cacheclient(QString connectPath, int _no)
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
			(char*)"-c", (char*)qPrintable(connectPath), 
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

int select_main_cacheclient(QList<CacheClientItem> &cacheClientList)
{
	for (int i=0; i<cacheClientList.count(); i++)
	{
		if (cacheClientList[i].initFlag == true)
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

QByteArray calculate_sec_ws_response_key_from_init_request(ZhttpRequestPacket &p)
{
	// parse request packet header
	HttpHeaders requestHeaders = p.headers;
	if (requestHeaders.contains("Sec-WebSocket-Key"))
	{
		QByteArray requestKey = requestHeaders.get("Sec-WebSocket-Key");
		QByteArray responseKey = QCryptographicHash::hash((requestKey + MAGIC_STRING), QCryptographicHash::Sha1).toBase64();

		log_debug("[WS] get ws response key for init request requestKey=%s responseKey=%s", requestKey.data(), responseKey.data());

		return respondKey;
	}
	return NULL;
}
