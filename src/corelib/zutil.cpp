/*
 * Copyright (C) 2015 Fanout, Inc.
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

#include "zutil.h"

#include <QStringList>
#include <QFile>
#include "qzmqsocket.h"

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
