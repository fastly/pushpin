/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "httpsessionupdatemanager.h"

#include <QUrl>
#include "timer.h"
#include "defercall.h"
#include "httpsession.h"

class HttpSessionUpdateManager::Private
{
public:
	class Bucket
	{
	public:
		QPair<int, QUrl> key;
		QSet<HttpSession*> sessions;
		QSet<HttpSession*> deferredSessions;
		std::unique_ptr<Timer> timer;
	};

	HttpSessionUpdateManager *q;
	QHash<QPair<int, QUrl>, Bucket*> buckets;
	QHash<Timer*, Bucket*> bucketsByTimer;
	QHash<HttpSession*, Bucket*> bucketsBySession;

	Private(HttpSessionUpdateManager *_q) :
		q(_q)
	{
	}

	~Private()
	{
		qDeleteAll(buckets);
	}

	void removeBucket(Bucket *bucket)
	{
		foreach(HttpSession *hs, bucket->sessions)
			bucketsBySession.remove(hs);

		bucketsByTimer.remove(bucket->timer.get());
		buckets.remove(bucket->key);
		delete bucket;
	}

	void registerSession(HttpSession *hs, int timeout, const QUrl &uri, bool resetTimeout)
	{
		QUrl tmp = uri;
		tmp.setQuery(QString()); // Remove the query part
		QPair<int, QUrl> key(timeout, tmp);

		Bucket *bucket = buckets.value(key);
		if(bucket)
		{
			if(bucket->sessions.contains(hs))
			{
				if(resetTimeout)
				{
					// Flag for later processing
					bucket->deferredSessions += hs;
				}
			}
			else
			{
				// Move the session to this bucket
				unregisterSession(hs);
				bucket->sessions += hs;
				bucketsBySession[hs] = bucket;
			}
		}
		else
		{
			// Bucket doesn't exist. Make it and put this session in it

			unregisterSession(hs);

			bucket = new Bucket;
			bucket->key = key;
			bucket->sessions += hs;
			bucket->timer = std::make_unique<Timer>();
			bucket->timer->timeout.connect(boost::bind(&Private::timer_timeout, this, bucket->timer.get()));

			buckets[key] = bucket;
			bucketsByTimer[bucket->timer.get()] = bucket;
			bucketsBySession[hs] = bucket;

			bucket->timer->start(timeout * 1000);
		}
	}

	void unregisterSession(HttpSession *hs)
	{
		Bucket *bucket = bucketsBySession.value(hs);
		if(!bucket)
			return;

		bucket->sessions.remove(hs);
		bucket->deferredSessions.remove(hs);
		bucketsBySession.remove(hs);

		if(bucket->sessions.isEmpty())
			removeBucket(bucket);
	}

private:
	void timer_timeout(Timer *timer)
	{
		Bucket *bucket = bucketsByTimer.value(timer);
		if(!bucket)
			return;

		QSet<HttpSession*> sessions;

		if(!bucket->deferredSessions.isEmpty())
		{
			foreach(HttpSession *hs, bucket->sessions)
			{
				if(!bucket->deferredSessions.contains(hs))
				{
					sessions += hs;
					bucketsBySession.remove(hs);
				}
			}

			bucket->sessions = bucket->deferredSessions;
			bucket->deferredSessions.clear();
			bucket->timer->start();
		}
		else
		{
			sessions = bucket->sessions;
			removeBucket(bucket);
		}

		foreach(HttpSession *hs, sessions)
			hs->update();
	}
};

HttpSessionUpdateManager::HttpSessionUpdateManager()
{
	d = new Private(this);
}

HttpSessionUpdateManager::~HttpSessionUpdateManager()
{
	delete d;
}

void HttpSessionUpdateManager::registerSession(HttpSession *hs, int timeout, const QUrl &uri, bool resetTimeout)
{
	d->registerSession(hs, timeout, uri, resetTimeout);
}

void HttpSessionUpdateManager::unregisterSession(HttpSession *hs)
{
	d->unregisterSession(hs);
}
