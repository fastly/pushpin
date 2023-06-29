/*
 * Copyright (C) 2015 Fanout, Inc.
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
