/*
 * Copyright (C) 2016-2021 Fanout, Inc.
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

#include "sequencer.h"

#include <QDateTime>
#include <QTimer>
#include "log.h"
#include "publishitem.h"
#include "publishlastids.h"

#define CHANNEL_PENDING_MAX 100
#define DEFAULT_PENDING_EXPIRE 5000
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

	class CachedId
	{
	public:
		qint64 expireTime;
		QString channel;
		QString id;
	};

	Sequencer *q;
	PublishLastIds *lastIds;
	QHash<QString, ChannelPendingItems> pendingItemsByChannel;
	QMap<QPair<qint64, PendingItem*>, PendingItem*> pendingItemsByTime;
	QTimer *expireTimer;
	int pendingExpireMSecs;
	int idCacheTtl;
	QHash<QPair<QString, QString>, CachedId*> idCacheById;
	QMap<QPair<qint64, CachedId*>, CachedId*> idCacheByExpireTime;

	Private(Sequencer *_q, PublishLastIds *_publishLastIds) :
		QObject(_q),
		q(_q),
		lastIds(_publishLastIds),
		pendingExpireMSecs(DEFAULT_PENDING_EXPIRE),
		idCacheTtl(-1)
	{
		expireTimer = new QTimer(this);
		connect(expireTimer, &QTimer::timeout, this, &Private::expireTimer_timeout);
	}

	~Private()
	{
		expireTimer->disconnect(this);
		expireTimer->setParent(0);
		expireTimer->deleteLater();

		qDeleteAll(idCacheById);
	}

	void addItem(const PublishItem &item, bool seq)
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		while(!idCacheByExpireTime.isEmpty())
		{
			QMap<QPair<qint64, CachedId*>, CachedId*>::iterator it = idCacheByExpireTime.begin();
			CachedId *i = it.value();

			if(i->expireTime > now)
				break;

			idCacheById.remove(QPair<QString, QString>(i->channel, i->id));
			idCacheByExpireTime.erase(it);
			delete i;
		}

		if(!item.id.isNull() && idCacheTtl > 0)
		{
			QPair<QString, QString> idKey(item.channel, item.id);
			if(idCacheById.contains(idKey))
			{
				// we've seen this ID recently, drop the message
				return;
			}

			CachedId *i = new CachedId;
			i->expireTime = now + (idCacheTtl * 1000);
			i->channel = item.channel;
			i->id = item.id;
			idCacheById.insert(idKey, i);
			idCacheByExpireTime.insert(QPair<qint64, CachedId*>(i->expireTime, i), i);
		}

		if(!seq)
		{
			emit q->itemReady(item);
			return;
		}

		QString lastId = lastIds->value(item.channel);

		if(!lastId.isNull() && !item.prevId.isNull() && lastId != item.prevId)
		{
			ChannelPendingItems &channelPendingItems = pendingItemsByChannel[item.channel];

			if(channelPendingItems.itemsByPrevId.contains(item.prevId))
			{
				log_debug("sequencer: already have item for channel [%s] depending on prev-id [%s], dropping", qPrintable(item.channel), qPrintable(item.prevId));
				return;
			}

			if(channelPendingItems.itemsByPrevId.count() >= CHANNEL_PENDING_MAX)
			{
				log_debug("sequencer: too many pending items for channel [%s], dropping", qPrintable(item.channel));
				return;
			}

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

	void clear(const QString &channel)
	{
		if(!pendingItemsByChannel.contains(channel))
			return;

		ChannelPendingItems &channelPendingItems = pendingItemsByChannel[channel];
		QHashIterator<QString, PendingItem*> it(channelPendingItems.itemsByPrevId);
		while(it.hasNext())
		{
			it.next();
			PendingItem *i = it.value();

			pendingItemsByTime.remove(QPair<qint64, PendingItem*>(i->time, i));
		}

		pendingItemsByChannel.remove(channel);
	}

	void sendItem(const PublishItem &item)
	{
		if(!item.id.isNull())
			lastIds->set(item.channel, item.id);
		else
			lastIds->remove(item.channel);

		emit q->itemReady(item);

		if(pendingItemsByChannel.contains(item.channel))
		{
			ChannelPendingItems &channelPendingItems = pendingItemsByChannel[item.channel];
			QString id = item.id;

			while(!id.isNull() && !channelPendingItems.itemsByPrevId.isEmpty())
			{
				PendingItem *i = channelPendingItems.itemsByPrevId.value(id);
				if(!i)
					break;

				PublishItem pitem = i->item;
				channelPendingItems.itemsByPrevId.remove(i->item.prevId);
				pendingItemsByTime.remove(QPair<qint64, PendingItem*>(i->time, i));
				delete i;

				if(!pitem.id.isNull())
					lastIds->set(pitem.channel, pitem.id);
				else
					lastIds->remove(pitem.channel);

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
		qint64 threshold = now - pendingExpireMSecs;

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

Sequencer::Sequencer(PublishLastIds *publishLastIds, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, publishLastIds);
}

Sequencer::~Sequencer()
{
	delete d;
}

void Sequencer::setWaitMax(int msecs)
{
	d->pendingExpireMSecs = msecs;
}

void Sequencer::setIdCacheTtl(int secs)
{
	d->idCacheTtl = secs;
}

void Sequencer::addItem(const PublishItem &item, bool seq)
{
	d->addItem(item, seq);
}

void Sequencer::clearPendingForChannel(const QString &channel)
{
	d->clear(channel);
}

#include "sequencer.moc"
