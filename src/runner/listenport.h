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

#ifndef LISTENPORT_H
#define LISTENPORT_H

#include <QHostAddress>

class ListenPort
{
public:
	QHostAddress addr;
	int port;
	bool ssl;
	QString localPath;
	int mode;
	QString user;
	QString group;

	ListenPort() :
		port(-1),
		ssl(false),
		mode(-1)
	{
	}

	ListenPort(const QHostAddress &_addr, int _port, bool _ssl, const QString &_localPath = QString(), int _mode = -1, const QString &_user = QString(), const QString &_group = QString()) :
		addr(_addr),
		port(_port),
		ssl(_ssl),
		localPath(_localPath),
		mode(_mode),
		user(_user),
		group(_group)
	{
	}
};

#endif
