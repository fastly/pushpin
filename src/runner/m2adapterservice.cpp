/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "m2adapterservice.h"

#include <QDir>
#include <QVariantList>
#include <QProcess>
#include "log.h"
#include "template.h"

M2AdapterService::M2AdapterService(
	const QString &binFile,
	const QString &configTemplateFile,
	const QString &runDir,
	const QString &logDir,
	const QString &ipcPrefix,
	const QString &filePrefix,
	int logLevel,
	const QList<int> &ports,
	QObject *parent) :
	Service(parent)
{
	args_ += binFile;
	args_ += "--config=" + QDir(runDir).filePath(filePrefix + "m2adapter.conf");

	if(!logDir.isEmpty())
	{
		args_ += "--logfile=" + QDir(logDir).filePath(filePrefix + "m2adapter.log");
		setStandardOutputFile(QProcess::nullDevice());
	}

	if(logLevel >= 0)
		args_ += "--loglevel=" + QString::number(logLevel);

	configTemplateFile_ = configTemplateFile;
	runDir_ = runDir;
	ipcPrefix_ = ipcPrefix;
	filePrefix_ = filePrefix;
	ports_ = ports;

	setName("m2a");
	setPidFile(QDir(runDir).filePath(filePrefix + "m2adapter.pid"));
}

QStringList M2AdapterService::arguments() const
{
	return args_;
}

bool M2AdapterService::acceptSighup() const
{
	return true;
}

bool M2AdapterService::preStart()
{
	QVariantList portStrs;
	foreach(int port, ports_)
		portStrs += QString::number(port);

	QVariantMap context;
	context["ports"] = portStrs;
	context["rundir"] = runDir_;
	context["ipc_prefix"] = ipcPrefix_;

	QString error;
	if(!Template::renderFile(configTemplateFile_, QDir(runDir_).filePath(filePrefix_ + "m2adapter.conf"), context, &error))
	{
		log_error("Failed to generate m2adapter config file: %s", qPrintable(error));
		return false;
	}

	return true;
}
