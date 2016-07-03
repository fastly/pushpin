/*
 * Copyright (C) 2016 Fanout, Inc.
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
	int port,
	bool ssl,
	QObject *parent) :
	Service(parent)
{
	args_ += binFile;
	args_ += configFile;
	args_ += serverName;

	name_ = QString("m2 %1:%2").arg(ssl ? "https" : "http", QString::number(port));
}

bool Mongrel2Service::generateConfigFile(const QString &m2shBinFile, const QString &configTemplateFile, const QString &runDir, const QString &logDir, const QString &certsDir, const QList<Interface> &interfaces)
{
	QVariantList ports;

	foreach(const Interface &i, interfaces)
	{
		QVariantMap port;
		port["value"] = i.port;
		port["addr"] = (!i.addr.isNull() ? i.addr.toString() : QString("0.0.0.0"));
		port["ssl"] = i.ssl;
		ports.append(port);
	}

	QVariantMap context;
	context["ports"] = ports;
	context["rundir"] = runDir;
	context["logdir"] = logDir;
	context["certdir"] = certsDir;

	QString error;
	if(!Template::renderFile(configTemplateFile, QDir(runDir).filePath("mongrel2.conf"), context))
	{
		log_error("Failed to generate mongrel2 config file: %s", qPrintable(error));
		return false;
	}

	QStringList args;
	args << "load";
	args << "-config" << QDir(runDir).filePath("mongrel2.conf");
	args << "-db" << QDir(runDir).filePath("mongrel2.sqlite");

	int ret = QProcess::execute(m2shBinFile, args);
	if(ret != 0)
	{
		log_error("Failed to run m2sh");
		return false;
	}

	return true;
}

QString Mongrel2Service::name() const
{
	return name_;
}

QStringList Mongrel2Service::arguments() const
{
	return args_;
}

bool Mongrel2Service::acceptSighup() const
{
	return true;
}
