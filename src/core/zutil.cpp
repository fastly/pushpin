/*
 * Copyright (C) 2015 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#include "zutil.h"

#include <QFile>
#include "qzmqsocket.h"
#include "acstring.h"

namespace ZUtil {

bool bindSpec(QZmq::Socket *sock, const QString &spec, int ipcFileMode, QString *errorMessage)
{
	if(!sock->bind(spec))
	{
		if(errorMessage)
			*errorMessage = QString("unable to bind to %1").arg(spec);
		return false;
	}

	if(spec.startsWith("ipc://") && ipcFileMode != -1)
	{
		QFile::Permissions perms;
		if(ipcFileMode & 0400)
			perms |= QFile::ReadUser;
		if(ipcFileMode & 0200)
			perms |= QFile::WriteUser;
		if(ipcFileMode & 0100)
			perms |= QFile::ExeUser;
		if(ipcFileMode & 0040)
			perms |= QFile::ReadGroup;
		if(ipcFileMode & 0020)
			perms |= QFile::WriteGroup;
		if(ipcFileMode & 0010)
			perms |= QFile::ExeGroup;
		if(ipcFileMode & 0004)
			perms |= QFile::ReadOther;
		if(ipcFileMode & 0002)
			perms |= QFile::WriteOther;
		if(ipcFileMode & 0001)
			perms |= QFile::ExeOther;
		if(!QFile::setPermissions(spec.mid(6), perms))
		{
			if(errorMessage)
				*errorMessage = QString("unable to set permissions on %1").arg(spec);
			return false;
		}
	}

	if(errorMessage)
		*errorMessage = QString();

	return true;
}

bool setupSocket(QZmq::Socket *sock, const QStringList &specs, bool bind, int ipcFileMode, QString *errorMessage)
{
	if(bind)
	{
		if(!bindSpec(sock, specs[0], ipcFileMode, errorMessage))
			return false;
	}
	else
	{
		foreach(const QString &spec, specs)
			sock->connectToAddress(spec);
	}

	if(errorMessage)
		*errorMessage = QString();

	return true;
}

bool setupSocket(QZmq::Socket *sock, const QString &spec, bool bind, int ipcFileMode, QString *errorMessage)
{
	return setupSocket(sock, QStringList() << spec, bind, ipcFileMode, errorMessage);
}

}
