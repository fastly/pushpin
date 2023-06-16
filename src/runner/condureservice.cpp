/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

#include "condureservice.h"

#include <QDir>
#include <QVariantList>
#include <QProcess>
#include <QUrl>
#include "log.h"
#include "template.h"

CondureService::CondureService(
	const QString &name,
	const QString &binFile,
	const QString &runDir,
	const QString &logDir,
	const QString &ipcPrefix,
	const QString &filePrefix,
	int logLevel,
	const QString &certsDir,
	int clientBufferSize,
	int maxconn,
	bool allowCompression,
	const QList<ListenPort> &ports,
	bool enableClient,
	QObject *parent) :
	Service(parent)
{
	args_ += binFile;

	if(!logDir.isEmpty())
	{
		setStandardOutputFile(QDir(logDir).filePath(filePrefix + name + ".log"));
	}

	if(logLevel >= 0)
		args_ += "--log-level=" + QString::number(logLevel);

	args_ += "--buffer-size=" + QString::number(clientBufferSize);

	args_ += "--stream-maxconn=" + QString::number(maxconn);

	if(allowCompression)
		args_ += "--compression";

	if(!ports.isEmpty())
	{
		// server mode

		bool usingSsl = false;

		foreach(const ListenPort &p, ports)
		{
			if(!p.localPath.isEmpty())
			{
				QString arg = "--listen=" + p.localPath + ",local,stream";

				if(p.mode >= 0)
					arg += ",mode=" + QString::number(p.mode, 8);

				if(!p.user.isEmpty())
					arg += ",user=" + p.user;

				if(!p.group.isEmpty())
					arg += ",group=" + p.group;

				args_ += arg;
			}
			else
			{
				QUrl url;
				url.setHost(!p.addr.isNull() ? p.addr.toString() : QString("0.0.0.0"));
				url.setPort(p.port);

				QString arg = "--listen=" + url.authority() + ",stream";

				if(p.ssl)
				{
					usingSsl = true;

					arg += ",tls,default-cert=default_" + QString::number(p.port);
				}

				args_ += arg;
			}
		}

		args_ += "--zclient-stream=ipc://" + runDir + "/" + ipcPrefix + "condure";

		if(usingSsl)
			args_ += "--tls-identities-dir=" + certsDir;
	}

	if(enableClient)
	{
		// client mode

		args_ += "--zserver-stream=ipc://" + runDir + "/" + ipcPrefix + "condure-client";

		args_ += "--deny-out-internal";
	}

	setName(name);
	setPidFile(QDir(runDir).filePath(filePrefix + name + ".pid"));
}

QStringList CondureService::arguments() const
{
	return args_;
}

bool CondureService::hasClientMode(const QString &binFile)
{
	QProcess proc;

	proc.start(binFile, QStringList() << "--help");

	if(!proc.waitForFinished(-1))
	{
		log_error("Failed to run condure: process error: %d", proc.error());
		return false;
	}

	if(proc.exitStatus() != QProcess::NormalExit)
	{
		log_error("Failed to run condure: process did not exit normally");
		return false;
	}

	int code = proc.exitCode();
	if(proc.exitCode() != 0)
	{
		log_error("Condure returned non-zero status: %d", code);
		return false;
	}

	QByteArray output = proc.readAllStandardOutput();
	return output.contains("--zserver-stream");
}
