/*
 * Copyright (C) 2016-2019 Fanout, Inc.
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

	if(logDir.length() > 0) {
		setStandardOutputFile(QDir(logDir).filePath(filePrefix + "mongrel2_" + QString::number(port) + ".log"));
	}
}

bool Mongrel2Service::generateConfigFile(const QString &m2shBinFile, const QString &configTemplateFile, const QString &runDir, const QString &logDir, const QString &ipcPrefix, const QString &filePrefix, const QString &certsDir, int clientBufferSize, const QList<Interface> &interfaces, int logLevel)
{
	QVariantList vinterfaces;

	foreach(const Interface &i, interfaces)
	{
		QVariantMap v;
		v["addr"] = (!i.addr.isNull() ? i.addr.toString() : QString("0.0.0.0"));
		v["port"] = i.port;
		v["ssl"] = i.ssl;
		vinterfaces += v;
	}

	QVariantMap context;
	context["interfaces"] = vinterfaces;
	context["rundir"] = runDir;
	context["logdir"] = logDir;
	context["certdir"] = certsDir;
	context["ipc_prefix"] = ipcPrefix;
	context["file_prefix"] = filePrefix;
	context["limits_buffer_size"] = clientBufferSize;
	context["disable_access_logging"] = (logLevel >= LOG_LEVEL_INFO) ? 0 : 1;

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


QString Mongrel2Service::filterLogLine(const int level, const QDateTime &time, const QString &line) const
{
	if(level > logLevel_) {
		return QString();
	}
	if(level == LOG_LEVEL_DEBUG) {
		return "[DEBUG] " + time.toString("yyyy-MM-dd HH:mm:ss.zzz") + " " + line;
	}
	if(level == LOG_LEVEL_INFO) {
		return "[INFO] " + time.toString("yyyy-MM-dd HH:mm:ss.zzz") + " " + line;
	}
	if(level == LOG_LEVEL_WARNING) {
		return "[WARN] " + time.toString("yyyy-MM-dd HH:mm:ss.zzz") + " " + line;
	}
	return "[ERR] " + time.toString("yyyy-MM-dd HH:mm:ss.zzz") + " " + line;
}

QString Mongrel2Service::formatLogLine(const QString &line) const
{
	if(line.isEmpty()) {
		return line;
	}

	bool isTnetString;
	QVariant data = TnetString::toVariant(qPrintable(line), 0, &isTnetString);
	// if line is a valid tnet string, so it's most probably an access log entry
	if(isTnetString)
	{
		QDateTime time = QDateTime::currentDateTime();
		return filterLogLine(LOG_LEVEL_INFO, time, line);
	}

	int at = line.indexOf('[');
        if(at == -1) {
		QDateTime time = QDateTime::currentDateTime();
		return filterLogLine(LOG_LEVEL_WARNING, time, "Can't parse mongrel2 log: " + line);
	}
	int end = line.indexOf(']', at);
	if(end == -1) {
		QDateTime time = QDateTime::currentDateTime();
		return filterLogLine(LOG_LEVEL_WARNING, time, "Can't parse mongrel2 log: " + line);
	}
	int level;
	if(line.midRef(at+1, end-at-1) == "INFO") {
		level = LOG_LEVEL_INFO;
	} else if(line.midRef(at+1, end-at-1) == "ERROR") {
		level = LOG_LEVEL_ERROR;
	} else if(line.midRef(at+1, end-at-1) == "WARN") {
		level = LOG_LEVEL_WARNING;
	} else {
		QDateTime time = QDateTime::currentDateTime();
		return filterLogLine(LOG_LEVEL_WARNING, time, "Can't parse severity: " + line);
	}

	// This may fail for leap seconds (ss = 60), as according to the qt documentation,
	// seconds in QDateTime::toString go from 00 to 59. But the time stamp is
	// created with strptime, where 60 is explicitly allowed ("The range is up to
	// 60 to allow for occasional leap seconds.")
	//
	// Also, this assumes that the locale of this process is the same as the one
	// of the process generating the time stamp, so that abbreviated day and month
	// names are the same.
	QDateTime time = QDateTime::fromString(line.left(at - 1), "ddd, dd MMM yyyy HH:mm:ss t");
	if(!time.isValid()) {
		QDateTime time = QDateTime::currentDateTime();
		return filterLogLine(LOG_LEVEL_WARNING, time, "Can't parse date: " + line);

	}
	return filterLogLine(level, time, line.mid(end + 1));
}
