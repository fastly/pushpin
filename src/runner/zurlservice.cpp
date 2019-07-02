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

#include "zurlservice.h"

#include <QDir>
#include <QProcess>
#include "log.h"
#include "template.h"

ZurlService::ZurlService(
	const QString &binFile,
	const QString &configTemplateFile,
	const QString &runDir,
	const QString &logDir,
	const QString &ipcPrefix,
	const QString &filePrefix,
	int logLevel,
	QObject *parent) :
	Service(parent)
{
	args_ += binFile;
	args_ += "--config=" + QDir(runDir).filePath(filePrefix + "zurl.conf");

	if(!logDir.isEmpty())
	{
		args_ += "--logfile=" + QDir(logDir).filePath(filePrefix + "zurl.log");
		setStandardOutputFile(QProcess::nullDevice());
	}

	if(logLevel >= 3)
		args_ += "--verbose";
	else
		args_ += "--loglevel=" + QString::number(logLevel);

	configTemplateFile_ = configTemplateFile;
	runDir_ = runDir;
	ipcPrefix_ = ipcPrefix;
	filePrefix_ = filePrefix;

	setName("zurl");
	setPidFile(QDir(runDir).filePath(filePrefix + "zurl.pid"));
}

QStringList ZurlService::arguments() const
{
	return args_;
}

bool ZurlService::acceptSighup() const
{
	return true;
}

bool ZurlService::preStart()
{
	QVariantMap context;
	context["rundir"] = runDir_;
	context["ipc_prefix"] = ipcPrefix_;

	QString error;
	if(!Template::renderFile(configTemplateFile_, QDir(runDir_).filePath(filePrefix_ + "zurl.conf"), context, &error))
	{
		log_error("Failed to generate zurl config file: %s", qPrintable(error));
		return false;
	}

	return true;
}
