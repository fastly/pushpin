/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

#include "connmgrservice.h"

#include <QDir>
#include <QVariantList>
#include <QProcess>
#include <QUrl>
#include "log.h"
#include "template.h"

ConnmgrService::ConnmgrService(
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
	bool enableClient)
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

		args_ += "--zclient-stream=ipc://" + runDir + "/" + ipcPrefix + "connmgr";

		if(usingSsl)
			args_ += "--tls-identities-dir=" + certsDir;
	}

	if(enableClient)
	{
		// client mode

		args_ += "--zserver-stream=ipc://" + runDir + "/" + ipcPrefix + "connmgr-client";

		args_ += "--deny-out-internal";
	}

	setName(name);
	setPidFile(QDir(runDir).filePath(filePrefix + name + ".pid"));
}

QStringList ConnmgrService::arguments() const
{
	return args_;
}

bool ConnmgrService::hasClientMode(const QString &binFile)
{
	QProcess proc;

	proc.start(binFile, QStringList() << "--help");

	if(!proc.waitForFinished(-1))
	{
		log_error("Failed to run connmgr: process error: %d", proc.error());
		return false;
	}

	if(proc.exitStatus() != QProcess::NormalExit)
	{
		log_error("Failed to run connmgr: process did not exit normally");
		return false;
	}

	int code = proc.exitCode();
	if(proc.exitCode() != 0)
	{
		log_error("connmgr returned non-zero status: %d", code);
		return false;
	}

	QByteArray output = proc.readAllStandardOutput();
	return output.contains("--zserver-stream");
}
