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

int cacheclient_get_no(ZhttpRequestPacket &p)
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

pid_t cacheclient_create_child_process(QString connectPath, int _no)
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
	log_debug("[WS] created new cache client%d parent=%d processId=%d", i, getpid(), processId);

	return processId;
}
