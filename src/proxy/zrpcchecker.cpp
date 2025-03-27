/*
 * Copyright (C) 2015 Fanout, Inc.
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

#include "zrpcchecker.h"

#include <assert.h>
#include <QHash>
#include "timer.h"
#include "zrpcrequest.h"

#define CHECK_TIMEOUT 8

using std::map;

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

	struct ZrpcReqConnections{
		Connection finishedConnection;
		Connection destroyedConnection;
	};

	ZrpcChecker *q;
	bool avail;
	std::unique_ptr<Timer> timer;
	QHash<ZrpcRequest*, Item*> requestsByReq;
	map<ZrpcRequest*, ZrpcReqConnections> reqConnectionMap;
	Connection timerConnection;

	Private(ZrpcChecker *_q) :
		q(_q),
		avail(true)
	{
		timer = std::make_unique<Timer>();
		timerConnection = timer->timeout.connect(boost::bind(&Private::timer_timeout, this));
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
			timerConnection.disconnect();
			timer.reset();
		}

		QHashIterator<ZrpcRequest*, Item*> it(requestsByReq);
		while(it.hasNext())
		{
			it.next();
			Item *i = it.value();
			reqConnectionMap.erase(i->req);
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

		reqConnectionMap[req] = {
			req->finished.connect(boost::bind(&Private::req_finished, this, req)),
			req->destroyed.connect(boost::bind(&Private::req_destroyed, this, req))
		};

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
			i->owned = true;
		}
		else
		{
			// if we aren't watching (or were watching, but no
			//   longer watching), then just delete what we were
			//   given
			reqConnectionMap.erase(req);
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

	void req_finished(ZrpcRequest *req)
	{
		Item *i = requestsByReq.value(req);
		assert(i);

		bool success = req->success();
		ZrpcRequest::ErrorCondition e = req->errorCondition();

		reqConnectionMap.erase(req);
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

	void req_destroyed(ZrpcRequest *req)
	{
		Item *i = requestsByReq.value(req);
		assert(i);

		reqConnectionMap.erase(req);
		requestsByReq.remove(req);
		delete i;
	}

	void timer_timeout()
	{
		avail = false;
	}
};

ZrpcChecker::ZrpcChecker()
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
