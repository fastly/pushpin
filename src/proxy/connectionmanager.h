/*
 * Copyright (C) 2015-2017 Fanout, Inc.
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

	// returns cid
	QByteArray addConnection(WebSocket *sock);

	// returns cid or empty
	QByteArray getConnection(WebSocket *sock) const;

	void removeConnection(WebSocket *sock);

	WsProxySession *getProxyForConnection(const QByteArray &cid) const;

	void setProxyForConnection(WebSocket *sock, WsProxySession *proxy);

private:
	class Private;
	Private *d;
};

#endif
