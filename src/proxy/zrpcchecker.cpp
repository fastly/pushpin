/*
 * Copyright (C) 2015 Fanout, Inc.
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

#include "zrpcchecker.h"

#include <assert.h>
#include <QHash>
#include <QTimer>
#include "zrpcrequest.h"

#define CHECK_TIMEOUT 8

class ZrpcChecker::Private : public QObject
{
	Q_OBJECT

public:
	class Item
	{
	public:
		ZrpcRequest *req;
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

	ZrpcChecker *q;
	bool avail;
	QTimer *timer;
	QHash<ZrpcRequest*, Item*> requestsByReq;

	Private(ZrpcChecker *_q) :
		QObject(_q),
		q(_q),
		avail(true)
	{
		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, &Private::timer_timeout);
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

		QHashIterator<ZrpcRequest*, Item*> it(requestsByReq);
		while(it.hasNext())
		{
			it.next();
			Item *i = it.value();
			i->req->disconnect(this);
			delete i;
		}

		requestsByReq.clear();
	}

	void restartCountdown()
	{
		timer->start(CHECK_TIMEOUT * 1000);
	}

	void watch(ZrpcRequest *req)
	{
		Item *i = requestsByReq.value(req);
		if(i)
			return; // already watching

		connect(req, &ZrpcRequest::finished, this, &Private::req_finished);
		connect(req, &ZrpcRequest::destroyed, this, &Private::req_destroyed);

		i = new Item;
		i->req = req;
		i->owned = false;
		requestsByReq.insert(req, i);

		// start the clock if we haven't yet
		if(!timer->isActive())
			restartCountdown();
	}

	void give(ZrpcRequest *req)
	{
		Item *i = requestsByReq.value(req);
		if(i)
		{
			// take over ownership
			req->setParent(this);
			i->owned = true;
		}
		else
		{
			// if we aren't watching (or were watching, but no
			//   longer watching), then just delete what we were
			//   given
			delete req;
		}
	}

	void handleSuccess()
	{
		avail = true;

		// success means we restart (or stop) the clock
		if(!requestsByReq.isEmpty())
			restartCountdown();
		else
			timer->stop();
	}

	void handleError()
	{
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

public slots:
	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();
		Item *i = requestsByReq.value(req);
		assert(i);

		bool success = req->success();
		ZrpcRequest::ErrorCondition e = req->errorCondition();

		req->disconnect(this);
		requestsByReq.remove(req);
		delete i;

		if(success)
		{
			handleSuccess();
		}
		else
		{
			if(e == ZrpcRequest::ErrorTimeout || e == ZrpcRequest::ErrorUnavailable)
			{
				handleError();
			}
			else
			{
				// any other error is fine, it means the inspector is responding
				handleSuccess();
			}
		}
	}

	void req_destroyed(QObject *obj)
	{
		Item *i = requestsByReq.value((ZrpcRequest *)obj);
		assert(i);

		requestsByReq.remove((ZrpcRequest *)obj);
		delete i;
	}

	void timer_timeout()
	{
		avail = false;
	}
};

ZrpcChecker::ZrpcChecker(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZrpcChecker::~ZrpcChecker()
{
	delete d;
}
        
bool ZrpcChecker::isInterfaceAvailable() const
{
	return d->avail;
}

void ZrpcChecker::setInterfaceAvailable(bool available)
{
	d->avail = available;
}
        
void ZrpcChecker::watch(ZrpcRequest *req)
{
	d->watch(req);
}

void ZrpcChecker::give(ZrpcRequest *req)
{
	d->give(req);
}

#include "zrpcchecker.moc"
