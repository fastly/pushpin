/*
 * Copyright (C) 2015 Fanout, Inc.
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

#include "cowstring.h"
#include "zmqsocket.h"
#include <QFile>

namespace ZUtil {

bool bindSpec(ZmqSocket *sock, const CowString &spec, int ipcFileMode, CowString *errorMessage) {
    if (!sock->bind(spec.asQString())) {
        if (errorMessage)
            *errorMessage = CowString("unable to bind to %1").arg(spec);
        return false;
    }

    if (spec.startsWith("ipc://") && ipcFileMode != -1) {
        QFile::Permissions perms;
        if (ipcFileMode & 0400)
            perms |= QFile::ReadUser;
        if (ipcFileMode & 0200)
            perms |= QFile::WriteUser;
        if (ipcFileMode & 0100)
            perms |= QFile::ExeUser;
        if (ipcFileMode & 0040)
            perms |= QFile::ReadGroup;
        if (ipcFileMode & 0020)
            perms |= QFile::WriteGroup;
        if (ipcFileMode & 0010)
            perms |= QFile::ExeGroup;
        if (ipcFileMode & 0004)
            perms |= QFile::ReadOther;
        if (ipcFileMode & 0002)
            perms |= QFile::WriteOther;
        if (ipcFileMode & 0001)
            perms |= QFile::ExeOther;
        if (!QFile::setPermissions(spec.mid(6).asQString(), perms)) {
            if (errorMessage)
                *errorMessage = CowString("unable to set permissions on %1").arg(spec);
            return false;
        }
    }

    if (errorMessage)
        *errorMessage = CowString();

    return true;
}

bool setupSocket(ZmqSocket *sock, const QStringList &specs, bool bind, int ipcFileMode,
                 CowString *errorMessage) {
    if (bind) {
        if (!bindSpec(sock, specs[0], ipcFileMode, errorMessage))
            return false;
    } else {
        foreach (const QString &spec, specs)
            sock->connectToAddress(spec);
    }

    if (errorMessage)
        *errorMessage = CowString();

    return true;
}

bool setupSocket(ZmqSocket *sock, const CowString &spec, bool bind, int ipcFileMode,
                 CowString *errorMessage) {
    return setupSocket(sock, QStringList() << spec.asQString(), bind, ipcFileMode, errorMessage);
}

} // namespace ZUtil
