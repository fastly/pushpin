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

#include "m2adapterservice.h"

#include <QDir>
#include <QVariantList>
#include "log.h"
#include "template.h"

M2AdapterService::M2AdapterService(
	const QString &binFile,
	const QString &configTemplateFile,
	const QString &runDir,
	const QString &logDir,
	bool verbose,
	const QList<int> &ports,
	QObject *parent) :
	Service(parent)
{
	args_ += binFile;
	args_ += "--config=" + QDir(runDir).filePath("m2adapter.conf");

	if(!logDir.isEmpty())
		args_ += "--logfile=" + QDir(logDir).filePath("m2adapter.log");

	if(verbose)
		args_ += "--verbose";

	configTemplateFile_ = configTemplateFile;
	runDir_ = runDir;
	ports_ = ports;
}

QString M2AdapterService::name() const
{
	return "m2a";
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
	QVariantList instances;
	foreach(int port, ports_)
	{
		QString portStr = QString::number(port);

		QVariantMap i;
		i["send_spec"] = QString("ipc://%1/pushpin-m2-out-%2").arg(runDir_, portStr);
		i["recv_spec"] = QString("ipc://%1/pushpin-m2-in-%2").arg(runDir_, portStr);
		i["send_ident"] = QString("pushpin-m2-%1").arg(portStr);
		i["control_spec"] = QString("ipc://%1/pushpin-m2-control-%2").arg(runDir_, portStr);
		instances.append(i);
	}

	QVariantMap context;
	context["instances"] = instances;
	context["rundir"] = runDir_;

	QString error;
	if(!Template::renderFile(configTemplateFile_, QDir(runDir_).filePath("m2adapter.conf"), context, &error))
	{
		log_error("Failed to generate m2adapter config file: %s", qPrintable(error));
		return false;
	}

	return true;
}
