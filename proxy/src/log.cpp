/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <QString>
#include <QTime>

static int g_level = LOG_LEVEL_DEBUG;
static QTime g_time;

static void log(int level, const char *fmt, va_list ap)
{
	if(!g_time.isValid())
		g_time.start();

	if(level <= g_level)
	{
		QString str;
		str.vsprintf(fmt, ap);

		const char *lstr;
		switch(level)
		{
			case LOG_LEVEL_ERROR:   lstr = "ERR"; break;
			case LOG_LEVEL_WARNING: lstr = "WARN"; break;
			case LOG_LEVEL_INFO:    lstr = "INFO"; break;
			case LOG_LEVEL_DEBUG:
			default:
				lstr = "DEBUG"; break;
		}

		QTime t(0, 0);
		t = t.addMSecs(g_time.elapsed());
		fprintf(stderr, "[%s] %s %s\n", lstr, qPrintable(t.toString("HH:mm:ss.zzz")), qPrintable(str));
	}
}

void log_setOutputLevel(int level)
{
	g_level = level;
}

void log_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log(LOG_LEVEL_ERROR, fmt, ap);
	va_end(ap);
}

void log_warning(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log(LOG_LEVEL_WARNING, fmt, ap);
	va_end(ap);
}

void log_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log(LOG_LEVEL_INFO, fmt, ap);
	va_end(ap);
}

void log_debug(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log(LOG_LEVEL_DEBUG, fmt, ap);
	va_end(ap);
}
