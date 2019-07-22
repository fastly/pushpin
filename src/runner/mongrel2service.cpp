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

#include <QDir>
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
	QObject *parent) :
	Service(parent)
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
