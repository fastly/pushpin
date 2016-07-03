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

#include "zurlservice.h"

#include <QDir>
#include "log.h"
#include "template.h"

ZurlService::ZurlService(
	const QString &binFile,
	const QString &configTemplateFile,
	const QString &runDir,
	const QString &logDir,
	bool verbose,
	QObject *parent) :
	Service(parent)
{
	args_ += binFile;
	args_ += "--config=" + QDir(runDir).filePath("zurl.conf");

	if(!logDir.isEmpty())
		args_ += "--logfile=" + QDir(logDir).filePath("zurl.log");

	if(verbose)
		args_ += "--verbose";

	configTemplateFile_ = configTemplateFile;
	runDir_ = runDir;
}

QString ZurlService::name() const
{
	return "zurl";
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

	QString error;
	if(!Template::renderFile(configTemplateFile_, QDir(runDir_).filePath("zurl.conf"), context, &error))
	{
		log_error("Failed to generate zurl config file: %s", qPrintable(error));
		return false;
	}

	return true;
}
