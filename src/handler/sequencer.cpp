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

#include "sequencer.h"

#include <QDateTime>
#include <QTimer>
#include "log.h"
#include "publishitem.h"
#include "publishlastids.h"

#define CHANNEL_PENDING_MAX 100
#define PENDING_EXPIRE 10000
#define EXPIRE_INTERVAL 1000

class Sequencer::Private : public QObject
{
	Q_OBJECT

public:
	class PendingItem
	{
	public:
		qint64 time;
		PublishItem item;
	};

	class ChannelPendingItems
	{
	public:
		QHash<QString, PendingItem*> itemsByPrevId;

		~ChannelPendingItems()
		{
			qDeleteAll(itemsByPrevId);
		}
	};

	Sequencer *q;
	PublishLastIds lastIds;
	QHash<QString, ChannelPendingItems> pendingItemsByChannel;
	QMap<QPair<qint64, PendingItem*>, PendingItem*> pendingItemsByTime;
	QTimer *expireTimer;

	Private(Sequencer *_q) :
		QObject(_q),
		q(_q),
		lastIds(1000000)
	{
		expireTimer = new QTimer(this);
		connect(expireTimer, &QTimer::timeout, this, &Private::expireTimer_timeout);
	}

	~Private()
	{
		expireTimer->disconnect(this);
		expireTimer->setParent(0);
		expireTimer->deleteLater();
	}

	void addItem(const PublishItem &item)
	{
		QString lastId = lastIds.value(item.channel);

		if(!item.prevId.isNull() && lastId != item.prevId)
		{
			ChannelPendingItems &channelPendingItems = pendingItemsByChannel[item.channel];

			if(channelPendingItems.itemsByPrevId.count() >= CHANNEL_PENDING_MAX)
			{
				log_debug("sequencer: too many pending items for channel [%s], dropping", qPrintable(item.channel));
				return;
			}

			qint64 now = QDateTime::currentMSecsSinceEpoch();

			PendingItem *i = new PendingItem;
			i->time = now;
			i->item = item;

			channelPendingItems.itemsByPrevId.insert(item.prevId, i);
			pendingItemsByTime.insert(QPair<qint64, PendingItem*>(i->time, i), i);

			if(!expireTimer->isActive())
				expireTimer->start(EXPIRE_INTERVAL);
			return;
		}

		sendItem(item);
	}

	void sendItem(const PublishItem &item)
	{
		lastIds.set(item.channel, item.id);
		emit q->itemReady(item);

		if(pendingItemsByChannel.contains(item.channel))
		{
			ChannelPendingItems &channelPendingItems = pendingItemsByChannel[item.channel];
			QString id = item.id;

			while(!channelPendingItems.itemsByPrevId.isEmpty())
			{
				PendingItem *i = channelPendingItems.itemsByPrevId.value(id);
				if(!i)
					break;

				PublishItem pitem = i->item;
				channelPendingItems.itemsByPrevId.remove(i->item.prevId);
				pendingItemsByTime.remove(QPair<qint64, PendingItem*>(i->time, i));
				delete i;

				lastIds.set(pitem.channel, pitem.id);
				emit q->itemReady(pitem);

				id = pitem.id;
			}

			if(channelPendingItems.itemsByPrevId.isEmpty())
			{
				pendingItemsByChannel.remove(item.channel);

				if(pendingItemsByChannel.isEmpty())
					expireTimer->stop();
			}
		}
	}

private slots:
	void expireTimer_timeout()
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();
		qint64 threshold = now - PENDING_EXPIRE;

		while(!pendingItemsByTime.isEmpty())
		{
			QMap<QPair<qint64, PendingItem*>, PendingItem*>::iterator it = pendingItemsByTime.begin();
			PendingItem *i = it.value();

			if(i->time > threshold)
				break;

			log_debug("timing out item channel=[%s] id=[%s]", qPrintable(i->item.channel), qPrintable(i->item.id));

			PublishItem item = i->item;
			ChannelPendingItems &channelPendingItems = pendingItemsByChannel[i->item.channel];
			channelPendingItems.itemsByPrevId.remove(i->item.prevId);
			pendingItemsByTime.erase(it);
			delete i;

			if(channelPendingItems.itemsByPrevId.isEmpty())
			{
				pendingItemsByChannel.remove(item.channel);

				if(pendingItemsByChannel.isEmpty())
					expireTimer->stop();
			}

			sendItem(item);
		}
	}
};

Sequencer::Sequencer(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

Sequencer::~Sequencer()
{
	delete d;
}

void Sequencer::addItem(const PublishItem &item)
{
	d->addItem(item);
}

#include "sequencer.moc"
