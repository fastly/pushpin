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

#include "connectionmanager.h"

#include <assert.h>
#include <QHash>
#include "uuidutil.h"

class ConnectionManager::Private
{
public:
	class Item
	{
	public:
		QPair<QByteArray, QByteArray> rid;
		WebSocket *sock;
		QByteArray cid;
		WsProxySession *proxy;

		Item() :
			sock(0),
			proxy(0)
		{
		}
	};

	QHash<WebSocket*, Item*> itemsBySock;
	QHash<QByteArray, Item*> itemsByCid;

	Private()
	{
	}

	~Private()
	{
		clearItemsBySock();
	}

	void clearItemsBySock()
	{
		qDeleteAll(itemsBySock);
		itemsBySock.clear();
		itemsByCid.clear();
	}
};

ConnectionManager::ConnectionManager()
{
	d = new Private;
}

ConnectionManager::~ConnectionManager()
{
	delete d;
}

QByteArray ConnectionManager::addConnection(WebSocket *sock)
{
	assert(!d->itemsBySock.contains(sock));

	Private::Item *i = new Private::Item;
	i->sock = sock;
	i->cid = UuidUtil::createUuid();
	d->itemsBySock[i->sock] = i;
	d->itemsByCid[i->cid] = i;

	return i->cid;
}

QByteArray ConnectionManager::getConnection(WebSocket *sock) const
{
	Private::Item *i = d->itemsBySock.value(sock);
	if(!i)
		return QByteArray();

	return i->cid;
}

void ConnectionManager::removeConnection(WebSocket *sock)
{
	Private::Item *i = d->itemsBySock.value(sock);
	assert(i);
	d->itemsBySock.remove(sock);
	d->itemsByCid.remove(i->cid);
	delete i;
}

WsProxySession *ConnectionManager::getProxyForConnection(const QByteArray &cid) const
{
	Private::Item *i = d->itemsByCid.value(cid);
	if(!i)
		return 0;

	return i->proxy;
}

void ConnectionManager::setProxyForConnection(WebSocket *sock, WsProxySession *proxy)
{
	Private::Item *i = d->itemsBySock.value(sock);
	assert(i);

	i->proxy = proxy;
}
