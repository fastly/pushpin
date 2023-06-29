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
