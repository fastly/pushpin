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

#include "logutil.h"

#include <assert.h>
#include <stdarg.h>
#include "qtcompat.h"
#include "tnetstring.h"
#include "log.h"

#define MAX_DATA_LENGTH 1000
#define MAX_CONTENT_LENGTH 1000

namespace LogUtil {

static QString trim(const QString &in, int max)
{
	if(in.length() > max && max >= 7)
		return in.mid(0, max / 2) + "..." + in.mid(in.length() - (max / 2) + 3);
	else
		return in;
}

static QByteArray trim(const QByteArray &in, int max)
{
	if(in.size() > max && max >= 7)
		return in.mid(0, max / 2) + "..." + in.mid(in.size() - (max / 2) + 3);
	else
		return in;
}

static QString makeLastIdsStr(const HttpHeaders &headers)
{
	QString out;

	bool first = true;
	foreach(const HttpHeaderParameters &params, headers.getAllAsParameters("Grip-Last"))
	{
		if(!first)
			out += ' ';
		out += QString("#%1=%2").arg(QString::fromUtf8(params[0].first.asQByteArray()), QString::fromUtf8(params.get("last-id").asQByteArray()));
		first = false;
	}

	return out;
}

static void logPacket(int level, const QString &message, const QVariant &data = QVariant(), int dataMax = -1, const QByteArray &content = QByteArray(), int contentMax = -1)
{
	QString out = message;

	if(data.isValid())
	{
		out += ' ' + trim(TnetString::variantToString(data, -1), dataMax);
	}

	if(!content.isNull())
	{
		out += ' ' + QString::number(content.size()) + ' ';
		QByteArray buf = trim(content, contentMax);
		out += TnetString::variantToString(QVariant(buf), -1);
	}

	log(level, "%s", qPrintable(out));
}

static void logPacket(int level, const QVariant &data, const char *fmt, va_list ap)
{
	logPacket(level, QString::vasprintf(fmt, ap), data, MAX_DATA_LENGTH);
}

static void logPacket(int level, const QByteArray &content, const char *fmt, va_list ap)
{
	logPacket(level, QString::vasprintf(fmt, ap), QVariant(), -1, content, MAX_CONTENT_LENGTH);
}

static void logPacket(int level, const QVariant &data, const QString &contentField, const char *fmt, va_list ap)
{
	QVariant meta;
	QByteArray content;

	if(typeId(data) == QMetaType::QVariantHash)
	{
		// Extract content. Meta is the remaining data
		QVariantHash hdata = data.toHash();
		content = hdata.value(contentField).toByteArray();
		hdata.remove(contentField);
		meta = hdata;
	}
	else
	{
		// If data isn't a hash, then we can't extract content, so
		// the meta part will be the entire data
		meta = data;
	}

	logPacket(level, QString::vasprintf(fmt, ap), meta, MAX_DATA_LENGTH, content, MAX_CONTENT_LENGTH);
}

void logVariant(int level, const QVariant &data, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	logPacket(level, data, fmt, ap);
	va_end(ap);
}

void logByteArray(int level, const QByteArray &content, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	logPacket(level, content, fmt, ap);
	va_end(ap);
}

void logVariantWithContent(int level, const QVariant &data, const QString &contentField, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	logPacket(level, data, contentField, fmt, ap);
	va_end(ap);
}

void logRequest(int level, const RequestData &data, const Config &config)
{
	QString msg = QString("%1 %2").arg(data.requestData.method, data.requestData.uri.toString(QUrl::FullyEncoded));

	if(!data.targetStr.isEmpty())
		msg += QString(" -> %1").arg(data.targetStr);

	if(data.requestData.uri.scheme() != "http" && data.requestData.uri.scheme() != "https" && data.targetOverHttp)
		msg += "[http]";

	if(config.fromAddress && !data.fromAddress.isNull())
		msg += QString(" from=%1").arg(data.fromAddress.toString());

	QUrl ref = QUrl(QString::fromUtf8(data.requestData.headers.get("Referer").asQByteArray()));
	if(!ref.isEmpty())
		msg += QString(" ref=%1").arg(ref.toString(QUrl::FullyEncoded));

	if(!data.routeId.isEmpty())
		msg += QString(" route=%1").arg(data.routeId);

	if(data.status == LogUtil::Response)
	{
		msg += QString(" code=%1 %2").arg(QString::number(data.responseData.code), QString::number(data.responseBodySize));
	}
	else if(data.status == LogUtil::Accept)
	{
		msg += " accept";
	}
	else
	{
		msg += " error";
	}

	if(data.retry)
		msg += " retry";

	if(data.sharedBy)
		msg += QString::asprintf(" shared=%p", data.sharedBy);

	if(config.userAgent)
	{
		QString userAgent = data.requestData.headers.get("User-Agent").asQByteArray();
		if(!userAgent.isEmpty())
			msg += QString(" ua=%1").arg(userAgent);
	}

	QString lastIdsStr = makeLastIdsStr(data.requestData.headers);
	if(!lastIdsStr.isEmpty())
		msg += ' ' + lastIdsStr;

	log(level, "%s", qPrintable(msg));
}

void logForRoute(const RouteInfo &routeInfo, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	QString msg = QString::vasprintf(fmt, ap);
	if(!routeInfo.id.isEmpty())
		msg += QString(" route=%1").arg(routeInfo.id);
	logPacket(routeInfo.logLevel, msg);
	va_end(ap);
}

}
