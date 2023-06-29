/*
 * Copyright (C) 2017 Fanout, Inc.
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

#ifndef LOGUTIL_H
#define LOGUTIL_H

#include <QHostAddress>
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"

namespace LogUtil {

enum RequestStatus
{
	Response,
	Accept,
	Error
};

class RequestData
{
public:
	QString routeId;
	RequestStatus status;
	HttpRequestData requestData;
	HttpResponseData responseData;
	int responseBodySize;
	QString targetStr;
	bool targetOverHttp;
	bool retry;
	void *sharedBy;
	QHostAddress fromAddress;

	RequestData() :
		status(Response),
		responseBodySize(-1),
		targetOverHttp(false),
		retry(false),
		sharedBy(0)
	{
	}
};

class Config
{
public:
	bool fromAddress;
	bool userAgent;

	Config() :
		fromAddress(false),
		userAgent(false)
	{
	}
};

void logVariant(int level, const QVariant &data, const char *fmt, ...);
void logByteArray(int level, const QByteArray &content, const char *fmt, ...);
void logVariantWithContent(int level, const QVariant &data, const QString &contentField, const char *fmt, ...);
void logRequest(int level, const RequestData &data, const Config &config = Config());

}

#endif
