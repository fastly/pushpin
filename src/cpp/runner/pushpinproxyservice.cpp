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

#include "pushpinproxyservice.h"

#include <QDir>
#include <QProcess>

PushpinProxyService::PushpinProxyService(
	const QString &binFile,
	const QString &configFile,
	const QString &runDir,
	const QString &logDir,
	const QString &ipcPrefix,
	const QString &filePrefix,
	int logLevel,
	const QStringList &routeLines,
	bool quietCheck)
{
	args_ += binFile;
	args_ += "--config=" + configFile;

	if(!ipcPrefix.isEmpty())
		args_ += "--ipc-prefix=" + ipcPrefix;

	if(!logDir.isEmpty())
	{
		args_ += "--logfile=" + QDir(logDir).filePath(filePrefix + "pushpin-proxy.log");
		setStandardOutputFile(QProcess::nullDevice());
	}

	if(logLevel >= 0)
		args_ += "--loglevel=" + QString::number(logLevel);

	foreach(const QString &route, routeLines)
		args_ += "--route=" + route;

	if(quietCheck)
		args_ += "--quiet-check";

	setName("proxy");
	setPidFile(QDir(runDir).filePath(filePrefix + "pushpin-proxy.pid"));
}

QStringList PushpinProxyService::arguments() const
{
	return args_;
}

bool PushpinProxyService::acceptSighup() const
{
	return true;
}
