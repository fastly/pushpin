/*
 * Copyright (C) 2015 Fanout, Inc.
 *
 * This file is part of Pushpin.
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
		QByteArray cid;
	};

	QHash<QPair<QByteArray, QByteArray>, Item*> itemsByRid;
};

ConnectionManager::ConnectionManager()
{
	d = new Private;
}

ConnectionManager::~ConnectionManager()
{
	delete d;
}

QByteArray ConnectionManager::addConnection(const QPair<QByteArray, QByteArray> &rid)
{
	assert(!d->itemsByRid.contains(rid));

	Private::Item *i = new Private::Item;
	i->rid = rid;
	i->cid = UuidUtil::createUuid();
	d->itemsByRid[i->rid] = i;

	return i->cid;
}

QByteArray ConnectionManager::getConnection(const QPair<QByteArray, QByteArray> &rid)
{
	Private::Item *i = d->itemsByRid.value(rid);
	if(!i)
		return QByteArray();

	return i->cid;
}

void ConnectionManager::removeConnection(const QPair<QByteArray, QByteArray> &rid)
{
	Private::Item *i = d->itemsByRid.value(rid);
	assert(i);
	d->itemsByRid.remove(rid);
}
