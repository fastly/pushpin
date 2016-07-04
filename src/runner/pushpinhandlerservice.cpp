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

#include "pushpinhandlerservice.h"

#include <QDir>
#include <QProcess>

PushpinHandlerService::PushpinHandlerService(
	const QString &binFile,
	const QString &configFile,
	const QString &runDir,
	const QString &logDir,
	bool verbose,
	QObject *parent) :
	Service(parent)
{
	args_ += binFile;
	args_ += "--config=" + configFile;

	if(!logDir.isEmpty())
	{
		args_ += "--logfile=" + QDir(logDir).filePath("pushpin-handler.log");
		setStandardOutputFile(QProcess::nullDevice());
	}

	if(verbose)
		args_ += "--verbose";

	setName("handler");
	setPidFile(QDir(runDir).filePath("pushpin-handler.pid"));
}

QStringList PushpinHandlerService::arguments() const
{
	return args_;
}

bool PushpinHandlerService::acceptSighup() const
{
	return true;
}
