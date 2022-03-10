/*
 * Copyright (C) 2016-2022 Fanout, Inc.
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

#include "mongrel2service.h"
#include "tnetstring.h"

#include <QDateTime>
#include <QDir>
#include <QVariant>
#include <QVariantList>
#include <QProcess>
#include "log.h"
#include "template.h"

Mongrel2Service::Mongrel2Service(
	const QString &binFile,
	const QString &configFile,
	const QString &serverName,
	const QString &runDir,
	const QString &logDir,
	const QString &filePrefix,
	int port,
	bool ssl,
	int logLevel,
	QObject *parent) :
	Service(parent),
	logLevel_(logLevel)
{
	args_ += binFile;
	args_ += configFile;
	args_ += serverName;

	setName(QString("m2 %1:%2").arg(ssl ? "https" : "http", QString::number(port)));

	// delete stale pid file, if any
	QString pidFile = QDir(runDir).filePath(filePrefix + "mongrel2_" + QString::number(port) + ".pid");
	QFile::remove(pidFile);

	if(!logDir.isEmpty())
	{
		setStandardOutputFile(QDir(logDir).filePath(filePrefix + "mongrel2_" + QString::number(port) + ".log"));
	}
}

bool Mongrel2Service::generateConfigFile(const QString &m2shBinFile, const QString &configTemplateFile, const QString &runDir, const QString &logDir, const QString &ipcPrefix, const QString &filePrefix, const QString &certsDir, int clientBufferSize, int maxconn, const QList<ListenPort> &ports, int logLevel)
{
	QVariantList vinterfaces;

	foreach(const ListenPort &p, ports)
	{
		if(!p.localPath.isEmpty())
		{
			log_error("Cannot use local_ports option with mongrel2");
			return false;
		}

		QVariantMap v;
		v["addr"] = (!p.addr.isNull() ? p.addr.toString() : QString("0.0.0.0"));
		v["port"] = p.port;
		v["ssl"] = p.ssl;
		vinterfaces += v;
	}

	QVariantMap context;
	context["interfaces"] = vinterfaces;
	context["rundir"] = runDir;
	context["logdir"] = logDir;
	context["loglevel"] = logLevel;
	context["certdir"] = certsDir;
	context["ipc_prefix"] = ipcPrefix;
	context["file_prefix"] = filePrefix;
	context["limits_buffer_size"] = clientBufferSize;
	context["disable_access_logging"] = (logLevel >= LOG_LEVEL_INFO) ? 0 : 1;
	context["superpoll_max_fd"] = maxconn + 1000;

	QString error;
	if(!Template::renderFile(configTemplateFile, QDir(runDir).filePath(filePrefix + "mongrel2.conf"), context, &error))
	{
		log_error("Failed to generate mongrel2 config file: %s", qPrintable(error));
		return false;
	}

	QStringList args;
	args << "load";
	args << "-config" << QDir(runDir).filePath(filePrefix + "mongrel2.conf");
	args << "-db" << QDir(runDir).filePath(filePrefix + "mongrel2.sqlite");

	int ret = QProcess::execute(m2shBinFile, args);
	if(ret != 0)
	{
		log_error("Failed to run m2sh");
		return false;
	}

	return true;
}

QStringList Mongrel2Service::arguments() const
{
	return args_;
}

bool Mongrel2Service::acceptSighup() const
{
	return true;
}


QString Mongrel2Service::filterLogLine(const int level, const QString &line) const
{
	if(level > logLevel_)
	{
		return QString();
	}
	QString tstr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
	switch(level)
	{
		case LOG_LEVEL_DEBUG:
			return "[DEBUG] " + tstr + " " + line;
		case LOG_LEVEL_INFO:
			return "[INFO] " + tstr + " " + line;
		case LOG_LEVEL_WARNING:
			return "[WARN] " + tstr + " " + line;
		default:
			return "[ERR] " + tstr + " " + line;
	}
}

QString Mongrel2Service::formatLogLine(const QString &line) const
{
	if(line.isEmpty())
	{
		return line;
	}

	TnetString::Type type;
	int dataOffset;
	int dataSize;
	bool isTnetString = TnetString::check(qPrintable(line), 0, &type, &dataOffset, &dataSize);
	// if line is a valid tnet string, it most probably is an access log entry
	if(isTnetString)
	{
		return filterLogLine(LOG_LEVEL_INFO, line);
	}

	int at = line.indexOf('[');
	int end;
	int level;
	if(at == -1)
	{
		QString debugTag("DEBUG");
		at = line.indexOf(debugTag);
		if(at == -1)
		{
			return filterLogLine(LOG_LEVEL_WARNING, "Can't parse mongrel2 log: " + line);
		}
		else
		{
			end = at + debugTag.length();
			level = LOG_LEVEL_DEBUG;
		}
	}
	else
	{
		end = line.indexOf(']', at);
		if(end == -1)
		{
			return filterLogLine(LOG_LEVEL_WARNING, "Can't parse mongrel2 log: " + line);
		}
		if(line.midRef(at + 1, end - at - 1) == "DEBUG")
		{
			level = LOG_LEVEL_DEBUG;
		}
		else if(line.midRef(at + 1, end - at - 1) == "INFO")
		{
			level = LOG_LEVEL_INFO;
		}
		else if(line.midRef(at + 1, end - at - 1) == "ERROR")
		{
			level = LOG_LEVEL_ERROR;
		}
		else if(line.midRef(at + 1, end - at - 1) == "WARN")
		{
			level = LOG_LEVEL_WARNING;
		}
		else
		{
			return filterLogLine(LOG_LEVEL_WARNING, "Can't parse severity: " + line);
		}
	}

	if(line.size() > end + 1 && line.at(end + 1) == ' ')
	{
		end++;
	}
	return filterLogLine(level, line.mid(end + 1));
}
