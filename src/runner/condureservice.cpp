/*
 * Copyright (C) 2020-2022 Fanout, Inc.
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
#include "log.h"
#include "template.h"

CondureService::CondureService(
	const QString &binFile,
	const QString &runDir,
	const QString &logDir,
	const QString &ipcPrefix,
	const QString &filePrefix,
	int logLevel,
	const QString &certsDir,
	int clientBufferSize,
	int maxconn,
	const QList<ListenPort> &ports,
	QObject *parent) :
	Service(parent)
{
	args_ += binFile;

	if(!logDir.isEmpty())
	{
		setStandardOutputFile(QDir(logDir).filePath(filePrefix + "condure.log"));
	}

	if(logLevel >= 0)
		args_ += "--log-level=" + QString::number(logLevel);

	bool usingSsl = false;

	foreach(const ListenPort &p, ports)
	{
		if(!p.localPath.isEmpty())
		{
			QString arg = "--listen=" + p.localPath + ",local,stream";

			args_ += arg;
		}
		else
		{
			QString addr = !p.addr.isNull() ? p.addr.toString() : QString("0.0.0.0");

			QString arg = "--listen=" + addr + ":" + QString::number(p.port) + ",stream";

			if(p.ssl)
			{
				usingSsl = true;

				arg += ",tls,default-cert=default_" + QString::number(p.port);
			}

			args_ += arg;
		}
	}

	args_ += "--zclient-stream=ipc://" + runDir + "/" + ipcPrefix + "condure";

	args_ += "--buffer-size=" + QString::number(clientBufferSize);

	args_ += "--stream-maxconn=" + QString::number(maxconn);

	if(usingSsl)
		args_ += "--tls-identities-dir=" + certsDir;

	setName("condure");
	setPidFile(QDir(runDir).filePath(filePrefix + "condure.pid"));
}

QStringList CondureService::arguments() const
{
	return args_;
}
