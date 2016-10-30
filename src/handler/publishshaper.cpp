/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "publishshaper.h"

// this class implements fair queueing per-route

#include <QPointer>
#include <QTimer>
#include "log.h"
#include "publishitem.h"

#define MIN_BATCH_INTERVAL 25

class PublishShaper::Private : public QObject
{
	Q_OBJECT

public:
	class Item
	{
	public:
		QPointer<QObject> target;
		PublishItem item;
		QList<QByteArray> exposeHeaders;
	};

	PublishShaper *q;
	int rate;
	int hwm;
	QMap<QString, QList<Item*> > itemsByRoute;
	QString lastRoute;
	QTimer *timer;
	bool firstPass;
	int batchInterval;
	int batchSize;
	bool lastBatchEmpty;

	Private(PublishShaper *_q) :
		QObject(_q),
		q(_q),
		rate(-1),
		hwm(-1),
		batchInterval(-1),
		batchSize(-1),
		lastBatchEmpty(false)
	{
		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, &Private::timeout);
	}

	~Private()
	{
		timer->disconnect(this);
		timer->setParent(0);
		timer->deleteLater();

		QMapIterator<QString, QList<Item*> > it(itemsByRoute);
		while(it.hasNext())
		{
			it.next();
			qDeleteAll(it.value());
		}
	}

	void setRate(int messagesPerSecond)
	{
		if(messagesPerSecond > 0)
		{
			rate = messagesPerSecond;

			if(rate >= 1000 / MIN_BATCH_INTERVAL)
			{
				batchInterval = MIN_BATCH_INTERVAL;
				batchSize = (rate * batchInterval + 999) / 1000;
			}
			else
			{
				batchInterval = 1000 / rate;
				batchSize = 1;
			}
		}
		else
		{
			rate = -1;
			batchInterval = -1;
			batchSize = -1;
		}

		setup();
	}

	bool add(QObject *target, const PublishItem &publishItem, const QString &route, const QList<QByteArray> &exposeHeaders)
	{
		QList<Item*> &items = itemsByRoute[route];
		if(hwm > 0 && items.count() >= hwm)
		{
			if(!route.isEmpty())
				log_warning("exceeded publish hwm (%d) for route %s, dropping message", hwm, qPrintable(route));
			else
				log_warning("exceeded publish hwm (%d), dropping message", hwm);

			return false;
		}

		Item *i = new Item;
		i->target = target;
		i->item = publishItem;
		i->exposeHeaders = exposeHeaders;
		items += i;

		setup();
		return true;
	}

	void setup()
	{
		if(rate > 0)
		{
			if(!itemsByRoute.isEmpty() || !lastBatchEmpty)
			{
				if(timer->isActive())
				{
					// after the first pass, switch to batch interval
					if(!firstPass)
						timer->setInterval(batchInterval);
				}
				else
				{
					// send first batch right away
					firstPass = true;
					timer->start(0);
				}
			}
			else
			{
				if(lastBatchEmpty)
				{
					// if we sent nothing on this pass, stop timer
					lastBatchEmpty = false;
					timer->stop();
				}
			}
		}
		else
		{
			if(!itemsByRoute.isEmpty())
			{
				if(timer->isActive())
				{
					// ensure we're on fastest interval
					timer->setInterval(0);
				}
				else
				{
					// send first batch right away
					firstPass = true;
					timer->start(0);
				}
			}
			else
			{
				timer->stop();
			}
		}
	}

private:
	// return false if self destroyed
	bool sendBatch()
	{
		if(itemsByRoute.isEmpty())
		{
			lastBatchEmpty = true;
			return true;
		}

		lastBatchEmpty = false;

		QMap<QString, QList<Item*> >::iterator it;

		if(!lastRoute.isNull())
		{
			it = itemsByRoute.find(lastRoute);

			if(it == itemsByRoute.end())
				it = itemsByRoute.begin();
		}
		else
		{
			it = itemsByRoute.begin();
		}

		QPointer<QObject> self = this;

		for(int n = 0; (batchSize < 1 || n < batchSize) && it != itemsByRoute.end(); ++n)
		{
			QList<Item*> &items = it.value();

			QString key = it.key();
			Item *i = items.takeFirst();

			if(items.isEmpty())
			{
				lastRoute = it.key();
				it = itemsByRoute.erase(it);
			}
			else
			{
				++it;
				if(it == itemsByRoute.end())
					it = itemsByRoute.begin();
			}

			QObject *target = i->target;
			PublishItem publishItem = i->item;
			QList<QByteArray> exposeHeaders = i->exposeHeaders;
			delete i;

			if(!target)
			{
				--n; // try again
				continue;
			}

			emit q->send(target, publishItem, exposeHeaders);
			if(!self)
				return false;
		}

		if(it != itemsByRoute.end())
			lastRoute = it.key();

		return true;
	}

private slots:
	void timeout()
	{
		if(!sendBatch())
			return;

		firstPass = false;

		setup();
	}
};

PublishShaper::PublishShaper(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

PublishShaper::~PublishShaper()
{
	delete d;
}

void PublishShaper::setRate(int messagesPerSecond)
{
	d->setRate(messagesPerSecond);
}

void PublishShaper::setHwm(int hwm)
{
	d->hwm = hwm;
}

bool PublishShaper::addMessage(QObject *target, const PublishItem &item, const QString &route, const QList<QByteArray> &exposeHeaders)
{
	QString r = (!route.isEmpty() ? route : QString("")); // index by empty string not null string
	return d->add(target, item, r, exposeHeaders);
}

#include "publishshaper.moc"
