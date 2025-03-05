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

#include "cache.h"

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

bool is_wsInitRequestFromCacheClient(ZhttpRequestPacket &p)
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
			if (number < gWsCacheClientList.count())
			{
				gWsCacheClientList[number].msgIdCount = 1;
				gWsCacheClientList[number].requestSeqCount = 1;
				gWsCacheClientList[number].responseSeqCount = 1;
				gWsCacheClientList[number].clientId = pId;
				log_debug("[CACHE_CLIENT] %s", gWsCacheClientList[number].clientId.data());

				// remove Socket-Owner header
				p.headers.removeAll("Socket-Owner");

				return true;
			}				
		}
	}

	return false;
}
