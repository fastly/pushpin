/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include <QTimer>
#include <QUrl>
#include "httpsession.h"

class HttpSessionUpdateManager::Private : public QObject
{
	Q_OBJECT

public:
	class Bucket
	{
	public:
		QPair<int, QUrl> key;
		QSet<HttpSession*> sessions;
		QSet<HttpSession*> deferredSessions;
		QTimer *timer;
	};

	HttpSessionUpdateManager *q;
	QHash<QPair<int, QUrl>, Bucket*> buckets;
	QHash<QTimer*, Bucket*> bucketsByTimer;
	QHash<HttpSession*, Bucket*> bucketsBySession;

	Private(HttpSessionUpdateManager *_q) :
		QObject(_q),
		q(_q)
	{
	}

	~Private()
	{
		QHashIterator<QPair<int, QUrl>, Bucket*> it(buckets);
		while(it.hasNext())
		{
			it.next();
			Bucket *bucket = it.value();

			bucket->timer->disconnect(this);
			bucket->timer->setParent(0);
			bucket->timer->deleteLater();
			delete bucket;
		}
	}

	void removeBucket(Bucket *bucket)
	{
		foreach(HttpSession *hs, bucket->sessions)
			bucketsBySession.remove(hs);

		bucketsByTimer.remove(bucket->timer);
		buckets.remove(bucket->key);

		bucket->timer->disconnect(this);
		bucket->timer->setParent(0);
		bucket->timer->deleteLater();
		delete bucket;
	}

	void registerSession(HttpSession *hs, int timeout, const QUrl &uri)
	{
		QUrl tmp = uri;
		tmp.setQuery(QString()); // remove the query part
		QPair<int, QUrl> key(timeout, tmp);

		Bucket *bucket = buckets.value(key);
		if(bucket)
		{
			if(bucket->sessions.contains(hs))
			{
				// if the session is already in this bucket, flag it
				//   for later processing
				bucket->deferredSessions += hs;
			}
			else
			{
				// move the session to this bucket
				unregisterSession(hs);
				bucket->sessions += hs;
				bucketsBySession[hs] = bucket;
			}
		}
		else
		{
			// bucket doesn't exist. make it and put this session in it

			unregisterSession(hs);

			bucket = new Bucket;
			bucket->key = key;
			bucket->sessions += hs;
			bucket->timer = new QTimer(this);
			connect(bucket->timer, &QTimer::timeout, this, &Private::timer_timeout);

			buckets[key] = bucket;
			bucketsByTimer[bucket->timer] = bucket;
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

private slots:
	void timer_timeout()
	{
		QTimer *timer = (QTimer *)sender();
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

HttpSessionUpdateManager::HttpSessionUpdateManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

HttpSessionUpdateManager::~HttpSessionUpdateManager()
{
	delete d;
}

void HttpSessionUpdateManager::registerSession(HttpSession *hs, int timeout, const QUrl &uri)
{
	d->registerSession(hs, timeout, uri);
}

void HttpSessionUpdateManager::unregisterSession(HttpSession *hs)
{
	d->unregisterSession(hs);
}

#include "httpsessionupdatemanager.moc"
