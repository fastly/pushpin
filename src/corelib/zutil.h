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

#ifndef ZUTIL_H
#define ZUTIL_H

class QString;
class QStringList;

namespace QZmq {

class Socket;

}

namespace ZUtil {

bool bindSpec(QZmq::Socket *sock, const QString &spec, int ipcFileMode, QString *errorMessage = 0);

bool setupSocket(QZmq::Socket *sock, const QStringList &specs, bool bind, int ipcFileMode, QString *errorMessage = 0);

bool setupSocket(QZmq::Socket *sock, const QString &spec, bool bind, int ipcFileMode, QString *errorMessage = 0);

}

#endif
