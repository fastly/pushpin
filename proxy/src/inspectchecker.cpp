/*
 * Copyright (C) 2012-2013 Fan Out Networks, Inc.
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

#include "inspectchecker.h"

#include <assert.h>
#include <QHash>
#include <QTimer>
#include "inspectrequest.h"

#define CHECK_TIMEOUT 8

class InspectChecker::Private : public QObject
{
	Q_OBJECT

public:
	class Item
	{
	public:
		InspectRequest *req;
		bool owned;

		Item() :
			req(0),
			owned(false)
		{
		}

		~Item()
		{
			if(owned)
				delete req;
		}
	};

	InspectChecker *q;
	bool avail;
	QTimer *timer;
	QHash<InspectRequest*, Item*> requestsByReq;

	Private(InspectChecker *_q) :
		QObject(_q),
		q(_q),
		avail(true)
	{
		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
		timer->setSingleShot(true);
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(timer)
		{
			timer->disconnect(this);
			timer->setParent(0);
			timer->deleteLater();
			timer = 0;
		}

		QHashIterator<InspectRequest*, Item*> it(requestsByReq);
		while(it.hasNext())
		{
			it.next();
			delete it.value();
		}

		requestsByReq.clear();
	}

	void restartCountdown()
	{
		timer->start(CHECK_TIMEOUT * 1000);
	}

	void watch(InspectRequest *req)
	{
		Item *i = requestsByReq.value(req);
		if(i)
			return; // already watching

		connect(req, SIGNAL(finished(const InspectData &)), SLOT(req_finished(const InspectData &)));
		connect(req, SIGNAL(error()), SLOT(req_error()));

		i = new Item;
		i->req = req;
		i->owned = false;
		requestsByReq.insert(req, i);

		// start the clock if we haven't yet
		if(!timer->isActive())
			restartCountdown();
	}

	void give(InspectRequest *req)
	{
		Item *i = requestsByReq.value(req);
		assert(i);

		i->owned = true;
	}

public slots:
	void req_finished(const InspectData &idata)
	{
		Q_UNUSED(idata);

		InspectRequest *req = (InspectRequest *)sender();
		Item *i = requestsByReq.value(req);
		assert(i);

		requestsByReq.remove(req);
		delete i;

		avail = true;

		// success means we restart (or stop) the clock
		if(!requestsByReq.isEmpty())
			restartCountdown();
		else
			timer->stop();
	}

	void req_error()
	{
		InspectRequest *req = (InspectRequest *)sender();
		Item *i = requestsByReq.value(req);
		assert(i);

		requestsByReq.remove(req);
		delete i;

		if(!requestsByReq.isEmpty())
		{
			// let the clock keep running
		}
		else
		{
			// stop clock and immediately indicate unavailability
			timer->stop();
			avail = false;
		}
	}

	void timer_timeout()
	{
		avail = false;
	}
};

InspectChecker::InspectChecker(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

InspectChecker::~InspectChecker()
{
	delete d;
}
        
bool InspectChecker::isInterfaceAvailable() const
{
	return d->avail;
}

void InspectChecker::setInterfaceAvailable(bool available)
{
	d->avail = available;
}
        
void InspectChecker::watch(InspectRequest *req)
{
	d->watch(req);
}

void InspectChecker::give(InspectRequest *req)
{
	d->give(req);
}

#include "inspectchecker.moc"
