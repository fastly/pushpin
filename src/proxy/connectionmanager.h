/*
 * Copyright (C) 2015-2017 Fanout, Inc.
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

#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include <QPair>
#include <QByteArray>

class WebSocket;
class WsProxySession;

class ConnectionManager
{
public:
	ConnectionManager();
	~ConnectionManager();

	// Returns cid
	QByteArray addConnection(WebSocket *sock);

	// Returns cid or empty
	QByteArray getConnection(WebSocket *sock) const;

	void removeConnection(WebSocket *sock);

	WsProxySession *getProxyForConnection(const QByteArray &cid) const;

	void setProxyForConnection(WebSocket *sock, WsProxySession *proxy);

private:
	class Private;
	Private *d;
};

#endif
