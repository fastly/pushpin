/*
 * Copyright (C) 2017 Fanout, Inc.
 *
 * This file is part of Pushpin.
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
 */

#include "log.h"

#include <assert.h>
#include <stdarg.h>
#include "tnetstring.h"

#define MAX_DATA_LENGTH 1000
#define MAX_CONTENT_LENGTH 1000

namespace LogUtil {

static QString trim(const QString &in, int max)
{
	if(in.length() > max && max >= 7)
		return in.mid(0, max / 2) + "..." + in.mid((max / 2) + 3);
	else
		return in;
}

static QByteArray trim(const QByteArray &in, int max)
{
	if(in.size() > max && max >= 7)
		return in.mid(0, max / 2) + "..." + in.mid((max / 2) + 3);
	else
		return in;
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
		QByteArray buf = trim(content, contentMax);
		out += ' ' + TnetString::variantToString(QVariant(buf), -1);
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

static void logPacket(int level, const QVariant &data, const QByteArray &content, const char *fmt, va_list ap)
{
	QString str;
	str.vsprintf(fmt, ap);
	logPacket(level, str, data, MAX_DATA_LENGTH, content, MAX_CONTENT_LENGTH);
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

void logPacket(int level, const QVariant &data, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	logPacket(level, data, fmt, ap);
	va_end(ap);
}

void logPacket(int level, const QByteArray &content, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	logPacket(level, content, fmt, ap);
	va_end(ap);
}

void logPacket(int level, const QVariant &data, const QByteArray &content, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	logPacket(level, data, content, fmt, ap);
	va_end(ap);
}

void logPacket(int level, const QVariant &data, const QString &contentField, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	logPacket(level, data, contentField, fmt, ap);
	va_end(ap);
}

}
