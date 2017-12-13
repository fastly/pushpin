/*
 * Copyright (C) 2017 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
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
