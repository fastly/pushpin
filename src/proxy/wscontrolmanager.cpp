/*
 * Copyright (C) 2014-2020 Fanout, Inc.
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

#include "wscontrolmanager.h"

#include <assert.h>
#include <QPointer>
#include <QDateTime>
#include <QTimer>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "log.h"
#include "tnetstring.h"
#include "zutil.h"
#include "logutil.h"
#include "wscontrolsession.h"

#define DEFAULT_HWM 101000

#define REFRESH_INTERVAL 1000
#define SESSION_EXPIRE 60000

#define SESSION_SHOULD_PROCESS (SESSION_EXPIRE * 3 / 4)
#define SESSION_MUST_PROCESS (SESSION_EXPIRE * 4 / 5)
#define SESSION_REFRESH_BUCKETS (SESSION_SHOULD_PROCESS / REFRESH_INTERVAL)

#define PACKET_ITEMS_MAX 128

class WsControlManager::Private : public QObject
{
	Q_OBJECT

public:
	class KeepAliveRegistration
	{
	public:
		WsControlSession *s;
		qint64 lastRefresh;
		int refreshBucket;
	};

	WsControlManager *q;
	int ipcFileMode;
	QString inSpec;
	QString outSpec;
	QZmq::Socket *inSock;
	QZmq::Socket *outSock;
	QZmq::Valve *inValve;
	QHash<QByteArray, WsControlSession*> sessionsByCid;
	QTimer *refreshTimer;
	QHash<WsControlSession*, KeepAliveRegistration*> keepAliveRegistrations;
	QMap<QPair<qint64, KeepAliveRegistration*>, KeepAliveRegistration*> sessionsByLastRefresh;
	QSet<KeepAliveRegistration*> sessionRefreshBuckets[SESSION_REFRESH_BUCKETS];
	int currentSessionRefreshBucket;

	Private(WsControlManager *_q) :
		QObject(_q),
		q(_q),
		ipcFileMode(-1),
		inSock(0),
		outSock(0),
		inValve(0),
		currentSessionRefreshBucket(0)
	{
		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, &Private::refresh_timeout);
	}

	~Private()
	{
		assert(sessionsByCid.isEmpty());
		assert(keepAliveRegistrations.isEmpty());

		refreshTimer->disconnect(this);
		refreshTimer->setParent(0);
		refreshTimer->deleteLater();
	}

	bool setupIn()
	{
		delete inSock;

		inSock = new QZmq::Socket(QZmq::Socket::Pull, this);

		inSock->setHwm(DEFAULT_HWM);

		QString errorMessage;
		if(!ZUtil::setupSocket(inSock, inSpec, true, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		inValve = new QZmq::Valve(inSock, this);
		connect(inValve, &QZmq::Valve::readyRead, this, &Private::in_readyRead);

		inValve->open();

		return true;
	}

	bool setupOut()
	{
		delete outSock;

		outSock = new QZmq::Socket(QZmq::Socket::Push, this);

		outSock->setHwm(DEFAULT_HWM);
		outSock->setShutdownWaitTime(0);

		QString errorMessage;
		if(!ZUtil::setupSocket(outSock, outSpec, true, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	int smallestSessionRefreshBucket()
	{
		int best = -1;
		int bestSize = 0;

		for(int n = 0; n < SESSION_REFRESH_BUCKETS; ++n)
		{
			if(best == -1 || sessionRefreshBuckets[n].count() < bestSize)
			{
				best = n;
				bestSize = sessionRefreshBuckets[n].count();
			}
		}

		return best;
	}

	void write(const WsControlPacket &packet)
	{
		assert(outSock);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariant(LOG_LEVEL_DEBUG, vpacket, "wscontrol: OUT");

		outSock->write(QList<QByteArray>() << buf);
	}

	void write(const WsControlPacket::Item &item)
	{
		WsControlPacket out;
		out.items += item;
		write(out);
	}

	void registerKeepAlive(WsControlSession *s)
	{
		if(keepAliveRegistrations.contains(s))
			return;

		qint64 now = QDateTime::currentMSecsSinceEpoch();

		KeepAliveRegistration *r = new KeepAliveRegistration;
		r->s = s;
		keepAliveRegistrations.insert(s, r);

		r->lastRefresh = now;
		sessionsByLastRefresh.insert(QPair<qint64, KeepAliveRegistration*>(r->lastRefresh, r), r);

		r->refreshBucket = smallestSessionRefreshBucket();
		sessionRefreshBuckets[r->refreshBucket] += r;

		setupKeepAlive();
	}

	void unregisterKeepAlive(WsControlSession *s)
	{
		KeepAliveRegistration *r = keepAliveRegistrations.value(s);
		if(!r)
			return;

		sessionRefreshBuckets[r->refreshBucket].remove(r);
		sessionsByLastRefresh.remove(QPair<qint64, KeepAliveRegistration*>(r->lastRefresh, r));
		keepAliveRegistrations.remove(s);
		delete r;

		setupKeepAlive();
	}

	void setupKeepAlive()
	{
		if(!keepAliveRegistrations.isEmpty())
		{
			if(!refreshTimer->isActive())
				refreshTimer->start(REFRESH_INTERVAL);
		}
		else
			refreshTimer->stop();
	}

private slots:
	void in_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("wscontrol: received message with parts != 1, skipping");
			return;
		}

		QVariant data = TnetString::toVariant(message[0]);
		if(data.isNull())
		{
			log_warning("wscontrol: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariant(LOG_LEVEL_DEBUG, data, "wscontrol: IN");

		WsControlPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("wscontrol: received message with invalid format (parse failed), skipping");
			return;
		}

		QPointer<QObject> self = this;

		foreach(const WsControlPacket::Item &i, p.items)
		{
			WsControlSession *s = sessionsByCid.value(i.cid);
			if(!s)
			{
				log_debug("wscontrol: received item for unknown connection id, canceling");

				// if this was not an error item, send cancel
				if(i.type != WsControlPacket::Item::Cancel)
				{
					WsControlPacket::Item out;
					out.cid = i.cid;
					out.type = WsControlPacket::Item::Cancel;
					write(out);
				}

				continue;
			}

			s->handle(i);

			if(!self)
				return;
		}
	}

	void refresh_timeout()
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		WsControlPacket packet;

		// process the current bucket
		const QSet<KeepAliveRegistration*> &bucket = sessionRefreshBuckets[currentSessionRefreshBucket];
		foreach(KeepAliveRegistration *r, bucket)
		{
			// move to the end
			QPair<qint64, KeepAliveRegistration*> k(r->lastRefresh, r);
			sessionsByLastRefresh.remove(k);
			r->lastRefresh = now;
			sessionsByLastRefresh.insert(QPair<qint64, KeepAliveRegistration*>(r->lastRefresh, r), r);

			WsControlPacket::Item i;
			i.cid = r->s->cid();
			i.type = WsControlPacket::Item::KeepAlive;
			i.ttl = SESSION_EXPIRE / 1000;
			packet.items += i;

			// if we're at max, send out now
			if(packet.items.count() >= PACKET_ITEMS_MAX)
			{
				write(packet);
				packet.items.clear();
			}
		}

		// process any others
		qint64 threshold = now - SESSION_MUST_PROCESS;
		while(!sessionsByLastRefresh.isEmpty())
		{
			QMap<QPair<qint64, KeepAliveRegistration*>, KeepAliveRegistration*>::iterator it = sessionsByLastRefresh.begin();
			KeepAliveRegistration *r = it.value();

			if(r->lastRefresh > threshold)
				break;

			// move to the end
			sessionsByLastRefresh.erase(it);
			r->lastRefresh = now;
			sessionsByLastRefresh.insert(QPair<qint64, KeepAliveRegistration*>(r->lastRefresh, r), r);

			WsControlPacket::Item i;
			i.cid = r->s->cid();
			i.type = WsControlPacket::Item::KeepAlive;
			i.ttl = SESSION_EXPIRE / 1000;
			packet.items += i;

			// if we're at max, send out now
			if(packet.items.count() >= PACKET_ITEMS_MAX)
			{
				write(packet);
				packet.items.clear();
			}
		}

		// send the rest
		if(!packet.items.isEmpty())
			write(packet);

		++currentSessionRefreshBucket;
		if(currentSessionRefreshBucket >= SESSION_REFRESH_BUCKETS)
			currentSessionRefreshBucket = 0;
	}
};

WsControlManager::WsControlManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

WsControlManager::~WsControlManager()
{
	delete d;
}

void WsControlManager::setIpcFileMode(int mode)
{
	d->ipcFileMode = mode;
}

bool WsControlManager::setInSpec(const QString &spec)
{
	d->inSpec = spec;
	return d->setupIn();
}

bool WsControlManager::setOutSpec(const QString &spec)
{
	d->outSpec = spec;
	return d->setupOut();
}

WsControlSession *WsControlManager::createSession(const QByteArray &cid)
{
	WsControlSession *s = new WsControlSession;
	s->setup(this, cid);
	return s;
}

void WsControlManager::link(WsControlSession *s, const QByteArray &cid)
{
	d->sessionsByCid.insert(cid, s);
}

void WsControlManager::unlink(const QByteArray &cid)
{
	d->sessionsByCid.remove(cid);
}

bool WsControlManager::canWriteImmediately() const
{
	assert(d->outSock);

	return d->outSock->canWriteImmediately();
}

void WsControlManager::write(const WsControlPacket::Item &item)
{
	d->write(item);
}

void WsControlManager::registerKeepAlive(WsControlSession *s)
{
	d->registerKeepAlive(s);
}

void WsControlManager::unregisterKeepAlive(WsControlSession *s)
{
	d->unregisterKeepAlive(s);
}

#include "wscontrolmanager.moc"
