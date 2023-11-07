/*
 * Copyright (C) 2016-2022 Fanout, Inc.
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

#if QT_VERSION >= 0x060000
		QStringView s = QStringView(line).mid(at + 1, end - at - 1);
#else
		QStringRef s = line.midRef(at + 1, end - at - 1);
#endif
		if(s.compare(QLatin1String("DEBUG")) == 0)
		{
			level = LOG_LEVEL_DEBUG;
		}
		else if(s.compare(QLatin1String("INFO")) == 0)
		{
			level = LOG_LEVEL_INFO;
		}
		else if(s.compare(QLatin1String("ERROR")) == 0)
		{
			level = LOG_LEVEL_ERROR;
		}
		else if(s.compare(QLatin1String("WARN")) == 0)
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
