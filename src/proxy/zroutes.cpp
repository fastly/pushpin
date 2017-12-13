/*
 * Copyright (C) 2014 Fanout, Inc.
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

#include "zroutes.h"

#include <assert.h>
#include <QHash>
#include <QStringList>
#include <QTimer>
#include "log.h"

static QStringList baseSpecToSpecs(const QString &baseSpec)
{
	int at = baseSpec.indexOf("://");
	assert(at != -1);

	at = baseSpec.indexOf(':', at + 3);
	if(at != -1) // probably a host:port spec
	{
		QString s = baseSpec.mid(0, at + 1);
		QString portStr = baseSpec.mid(at + 1);
		bool ok = false;
		int port = portStr.toInt(&ok);
		assert(ok);

		QStringList out;
		out += s + QString::number(port);
		out += s + QString::number(port + 1);
		out += s + QString::number(port + 2);
		return out;
	}
	else // probably a path spec
	{
		QStringList out;
		out += baseSpec + "-out";
		out += baseSpec + "-out-stream";
		out += baseSpec + "-in";
		return out;
	}
}

class ZRoutes::Private : public QObject
{
	Q_OBJECT

public:
	class Item
	{
	public:
		QString spec;
		ZhttpManager *manager;
		int refs;
		bool markedForRemoval;

		Item(const QString &_spec, ZhttpManager *_manager) :
			spec(_spec),
			manager(_manager),
			refs(0),
			markedForRemoval(false)
		{
		}

		~Item()
		{
			delete manager;
		}
	};

	ZRoutes *q;
	QByteArray instanceId;
	QStringList defaultOutSpecs;
	QStringList defaultOutStreamSpecs;
	QStringList defaultInSpecs;
	Item *defaultItem;
	QHash<QString, Item*> itemsBySpec;
	QHash<ZhttpManager*, Item*> itemsByManager;
	QTimer *cleanupTimer;

	Private(ZRoutes *_q) :
		QObject(_q),
		q(_q),
		defaultItem(0)
	{
		cleanupTimer = new QTimer(this);
		connect(cleanupTimer, &QTimer::timeout, this, &Private::removeUnused);
		cleanupTimer->setInterval(10000);
		cleanupTimer->start();
	}

	~Private()
	{
		qDeleteAll(itemsBySpec);
		delete defaultItem;

		cleanupTimer->disconnect(this);
		cleanupTimer->setParent(0);
		cleanupTimer->deleteLater();
	}

	Item *ensureDefaultItem()
	{
		if(!defaultItem)
		{
			ZhttpManager *manager = new ZhttpManager(this);
			manager->setInstanceId(instanceId);
			manager->setClientOutSpecs(defaultOutSpecs);
			manager->setClientOutStreamSpecs(defaultOutStreamSpecs);
			manager->setClientInSpecs(defaultInSpecs);

			defaultItem = new Item(QString(), manager);
		}

		return defaultItem;
	}

	Item *ensureItem(const DomainMap::ZhttpRoute &route)
	{
		Item *i = itemsBySpec.value(route.baseSpec);
		if(!i)
		{
			ZhttpManager *manager = new ZhttpManager(this);
			manager->setInstanceId(instanceId);
			manager->setIpcFileMode(route.ipcFileMode);
			manager->setBind(true);

			if(route.req)
			{
				manager->setClientReqSpecs(QStringList() << route.baseSpec);
			}
			else
			{
				QStringList specs = baseSpecToSpecs(route.baseSpec);
				manager->setClientOutSpecs(QStringList() << specs[0]);
				manager->setClientOutStreamSpecs(QStringList() << specs[1]);
				manager->setClientInSpecs(QStringList() << specs[2]);
			}

			i = new Item(route.baseSpec, manager);
			itemsBySpec.insert(route.baseSpec, i);
			itemsByManager.insert(manager, i);
		}

		return i;
	}

	void tryRemoveItem(Item *i)
	{
		if(i->refs == 0 && i->manager->connectionCount() == 0)
			removeItem(i);
		else
			i->markedForRemoval = true;
	}

	void removeItem(Item *i)
	{
		log_debug("zroutes: removing %s", qPrintable(i->spec));

		assert(i->refs == 0 && i->manager->connectionCount() == 0);
		itemsBySpec.remove(i->spec);
		itemsByManager.remove(i->manager);
		delete i;
	}

public slots:
	void removeUnused()
	{
		QList<Item*> toRemove;
		QHashIterator<QString, Item*> it(itemsBySpec);
		while(it.hasNext())
		{
			it.next();
			Item *i = it.value();

			if(i->markedForRemoval && i->refs == 0 && i->manager->connectionCount() == 0)
				toRemove += i;
		}

		foreach(Item *i, toRemove)
			removeItem(i);
	}
};

ZRoutes::ZRoutes(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZRoutes::~ZRoutes()
{
	delete d;
}

void ZRoutes::setInstanceId(const QByteArray &id)
{
	d->instanceId = id;
}

void ZRoutes::setDefaultOutSpecs(const QStringList &specs)
{
	d->defaultOutSpecs = specs;
}

void ZRoutes::setDefaultOutStreamSpecs(const QStringList &specs)
{
	d->defaultOutStreamSpecs = specs;
}

void ZRoutes::setDefaultInSpecs(const QStringList &specs)
{
	d->defaultInSpecs = specs;
}

void ZRoutes::setup(const QList<DomainMap::ZhttpRoute> &routes)
{
	d->ensureDefaultItem();

	QList<DomainMap::ZhttpRoute> toAdd;
	foreach(const DomainMap::ZhttpRoute &route, routes)
	{
		if(!d->itemsBySpec.contains(route.baseSpec))
			toAdd += route;
	}

	QStringList toRemove;
	QHashIterator<QString, Private::Item*> it(d->itemsBySpec);
	while(it.hasNext())
	{
		it.next();
		const QString &spec = it.key();

		bool found = false;
		foreach(const DomainMap::ZhttpRoute &route, routes)
		{
			if(route.baseSpec == spec)
			{
				found = true;
				break;
			}
		}
		if(!found)
			toRemove += spec;
	}

	foreach(const QString &spec, toRemove)
	{
		Private::Item *i = d->itemsBySpec.value(spec);
		assert(i);

		d->tryRemoveItem(i);
	}

	foreach(const DomainMap::ZhttpRoute &route, toAdd)
	{
		log_debug("zroutes: adding %s", qPrintable(route.baseSpec));
		d->ensureItem(route);
	}
}

ZhttpManager *ZRoutes::defaultManager()
{
	return d->ensureDefaultItem()->manager;
}

ZhttpManager *ZRoutes::managerForRoute(const DomainMap::ZhttpRoute &route)
{
	return d->ensureItem(route)->manager;
}

void ZRoutes::addRef(ZhttpManager *zhttpManager)
{
	Private::Item *i = (d->defaultItem->manager == zhttpManager ? d->defaultItem : d->itemsByManager.value(zhttpManager));
	assert(i);
	++(i->refs);
}

void ZRoutes::removeRef(ZhttpManager *zhttpManager)
{
	Private::Item *i = (d->defaultItem->manager == zhttpManager ? d->defaultItem : d->itemsByManager.value(zhttpManager));
	assert(i);
	assert(i->refs > 0);
	--(i->refs);
}

#include "zroutes.moc"
