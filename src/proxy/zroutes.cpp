/*
 * Copyright (C) 2014 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#include "zroutes.h"

#include <assert.h>
#include <QHash>
#include <QStringList>
#include <boost/signals2.hpp>
#include "log.h"
#include "timer.h"

using Connection = boost::signals2::scoped_connection;

static QStringList baseSpecToSpecs(const QString &baseSpec)
{
	int at = baseSpec.indexOf("://");
	assert(at != -1);

	at = baseSpec.indexOf(':', at + 3);
	if(at != -1) // Probably a host:port spec
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
	else // Probably a path spec
	{
		QStringList out;
		out += baseSpec + "-out";
		out += baseSpec + "-out-stream";
		out += baseSpec + "-in";
		return out;
	}
}

class ZRoutes::Private
{
public:
	class Item
	{
	public:
		QString spec;
		std::unique_ptr<ZhttpManager> manager;
		int refs;
		bool markedForRemoval;

		Item(const QString &_spec, std::unique_ptr<ZhttpManager> _manager) :
			spec(_spec),
			manager(std::move(_manager)),
			refs(0),
			markedForRemoval(false)
		{
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
	std::unique_ptr<Timer> cleanupTimer;
	Connection cleanupTimerConnection;

	Private(ZRoutes *_q) :
		q(_q),
		defaultItem(0)
	{
		cleanupTimer = std::make_unique<Timer>();
		cleanupTimerConnection = cleanupTimer->timeout.connect(boost::bind(&Private::removeUnused, this));
		cleanupTimer->setInterval(10000);
		cleanupTimer->start();
	}

	~Private()
	{
		qDeleteAll(itemsBySpec);
		delete defaultItem;
	}

	Item *ensureDefaultItem()
	{
		if(!defaultItem)
		{
			std::unique_ptr<ZhttpManager> manager = std::make_unique<ZhttpManager>();
			manager->setInstanceId(instanceId);
			manager->setClientOutSpecs(defaultOutSpecs);
			manager->setClientOutStreamSpecs(defaultOutStreamSpecs);
			manager->setClientInSpecs(defaultInSpecs);

			defaultItem = new Item(QString(), std::move(manager));
		}

		return defaultItem;
	}

	Item *ensureItem(const DomainMap::ZhttpRoute &route)
	{
		Item *i = itemsBySpec.value(route.baseSpec);
		if(!i)
		{
			std::unique_ptr<ZhttpManager> manager = std::make_unique<ZhttpManager>();
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

			i = new Item(route.baseSpec, std::move(manager));
			itemsBySpec.insert(route.baseSpec, i);
			itemsByManager.insert(i->manager.get(), i);
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
		itemsByManager.remove(i->manager.get());
		delete i;
	}

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

ZRoutes::ZRoutes()
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
	return d->ensureDefaultItem()->manager.get();
}

ZhttpManager *ZRoutes::managerForRoute(const DomainMap::ZhttpRoute &route)
{
	return d->ensureItem(route)->manager.get();
}

void ZRoutes::addRef(ZhttpManager *zhttpManager)
{
	Private::Item *i = (d->defaultItem->manager.get() == zhttpManager ? d->defaultItem : d->itemsByManager.value(zhttpManager));
	assert(i);
	++(i->refs);
}

void ZRoutes::removeRef(ZhttpManager *zhttpManager)
{
	Private::Item *i = (d->defaultItem->manager.get() == zhttpManager ? d->defaultItem : d->itemsByManager.value(zhttpManager));
	assert(i);
	assert(i->refs > 0);
	--(i->refs);
}
