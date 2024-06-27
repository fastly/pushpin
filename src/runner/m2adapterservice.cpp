/*
 * Copyright (C) 2016 Fanout, Inc.
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
	const QList<int> &ports)
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
