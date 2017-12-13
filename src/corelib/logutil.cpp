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

#include "logutil.h"

#include <assert.h>
#include <stdarg.h>
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
		out += QString("#%1=%2").arg(QString::fromUtf8(params[0].first), QString::fromUtf8(params.get("last-id")));
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
	QString str;
	str.vsprintf(fmt, ap);
	logPacket(level, str, data, MAX_DATA_LENGTH);
}

static void logPacket(int level, const QByteArray &content, const char *fmt, va_list ap)
{
	QString str;
	str.vsprintf(fmt, ap);
	logPacket(level, str, QVariant(), -1, content, MAX_CONTENT_LENGTH);
}

static void logPacket(int level, const QVariant &data, const QString &contentField, const char *fmt, va_list ap)
{
	QString str;
	str.vsprintf(fmt, ap);

	QVariant meta;
	QByteArray content;

	if(data.type() == QVariant::Hash)
	{
		// extract content. meta is the remaining data
		QVariantHash hdata = data.toHash();
		content = hdata.value(contentField).toByteArray();
		hdata.remove(contentField);
		meta = hdata;
	}
	else
	{
		// if data isn't a hash, then we can't extract content, so
		//   the meta part will be the entire data
		meta = data;
	}

	logPacket(level, str, meta, MAX_DATA_LENGTH, content, MAX_CONTENT_LENGTH);
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

	QUrl ref = QUrl(QString::fromUtf8(data.requestData.headers.get("Referer")));
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
		msg += QString().sprintf(" shared=%p", data.sharedBy);

	if(config.userAgent)
	{
		QString userAgent = data.requestData.headers.get("User-Agent");
		if(!userAgent.isEmpty())
			msg += QString(" ua=%1").arg(userAgent);
	}

	QString lastIdsStr = makeLastIdsStr(data.requestData.headers);
	if(!lastIdsStr.isEmpty())
		msg += ' ' + lastIdsStr;

	log(level, "%s", qPrintable(msg));
}

}
