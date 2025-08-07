/*
 * Copyright (C) 2014-2020 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#include "wscontrolmanager.h"

#include <assert.h>
#include <QDateTime>
#include <boost/signals2.hpp>
#include "cowbytearray.h"
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "qzmqreqmessage.h"
#include "log.h"
#include "timer.h"
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

using Connection = boost::signals2::scoped_connection;

class WsControlManager::Private
{
public:
	class KeepAliveRegistration
	{
	public:
		WsControlSession *s;
		qint64 lastRefresh;
		int refreshBucket;
	};

	WsControlManager *q;
	QByteArray identity;
	int ipcFileMode;
	QStringList initSpecs;
	QStringList streamSpecs;
	std::unique_ptr<QZmq::Socket> initSock;
	std::unique_ptr<QZmq::Socket> streamSock;
	std::unique_ptr<QZmq::Valve> streamValve;
	QHash<QByteArray, WsControlSession*> sessionsByCid;
	std::unique_ptr<Timer> refreshTimer;
	QHash<WsControlSession*, KeepAliveRegistration*> keepAliveRegistrations;
	QMap<QPair<qint64, KeepAliveRegistration*>, KeepAliveRegistration*> sessionsByLastRefresh;
	QSet<KeepAliveRegistration*> sessionRefreshBuckets[SESSION_REFRESH_BUCKETS];
	int currentSessionRefreshBucket;
	Connection streamValveConnection;
	Connection refreshTimerConnection;

	Private(WsControlManager *_q) :
		q(_q),
		ipcFileMode(-1),
		currentSessionRefreshBucket(0)
	{
		refreshTimer = std::make_unique<Timer>();
		refreshTimerConnection = refreshTimer->timeout.connect(boost::bind(&Private::refresh_timeout, this));
	}

	~Private()
	{
		assert(sessionsByCid.isEmpty());
		assert(keepAliveRegistrations.isEmpty());
	}

	bool setupInit()
	{
		initSock.reset();

		initSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Push);

		initSock->setHwm(DEFAULT_HWM);
		initSock->setShutdownWaitTime(0);

		foreach(const QString &spec, initSpecs)
		{
			QString errorMessage;
			if(!ZUtil::setupSocket(initSock.get(), spec, true, ipcFileMode, &errorMessage))
			{
				log_error("%s", qPrintable(errorMessage));
				return false;
			}
		}

		return true;
	}

	bool setupStream()
	{
		streamValve.reset();
		streamSock.reset();

		streamSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);

		streamSock->setIdentity(identity);
		streamSock->setHwm(DEFAULT_HWM);
		streamSock->setShutdownWaitTime(0);

		foreach(const QString &spec, streamSpecs)
		{
			QString errorMessage;
			if(!ZUtil::setupSocket(streamSock.get(), spec, true, ipcFileMode, &errorMessage))
			{
				log_error("%s", qPrintable(errorMessage));
				return false;
			}
		}

		streamValve = std::make_unique<QZmq::Valve>(streamSock.get());
		streamValveConnection = streamValve->readyRead.connect(boost::bind(&Private::stream_readyRead, this, boost::placeholders::_1));

		streamValve->open();

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

	void writeInit(const WsControlPacket &packet)
	{
		assert(streamSock);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariant(LOG_LEVEL_DEBUG, vpacket, "wscontrol: OUT");

		initSock->write(QList<QByteArray>() << buf);
	}

	void writeStream(const WsControlPacket &packet, const QByteArray &instanceAddress)
	{
		assert(streamSock);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariant(LOG_LEVEL_DEBUG, vpacket, "wscontrol: OUT to=%s", instanceAddress.data());

		QList<QByteArray> msg;
		msg += instanceAddress;
		msg += QByteArray();
		msg += buf;
		streamSock->write(msg);
	}

	void writeInit(const WsControlPacket::Item &item)
	{
		WsControlPacket out;
		out.from = identity;
		out.items += item;
		writeInit(out);
	}

	void writeStream(const WsControlPacket::Item &item, const QByteArray &instanceAddress)
	{
		WsControlPacket out;
		out.from = identity;
		out.items += item;
		writeStream(out, instanceAddress);
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

private:
	void stream_readyRead(const QList<QByteArray> &message)
	{
		QZmq::ReqMessage req(message);

		if(req.content().count() != 1)
		{
			log_warning("wscontrol: received message with parts != 1, skipping");
			return;
		}

		QVariant data = TnetString::toVariant(req.content()[0].asQByteArray());
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

		if(p.from.isEmpty())
		{
			log_warning("wscontrol: received message with invalid from value, skipping");
			return;
		}

		std::weak_ptr<Private> self = q->d;

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
					writeStream(out, p.from);
				}

				continue;
			}

			s->handle(p.from, i);

			if(self.expired())
				return;
		}
	}

	void refresh_timeout()
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		QHash<QByteArray, WsControlPacket> packets;

		// process the current bucket
		const QSet<KeepAliveRegistration*> &bucket = sessionRefreshBuckets[currentSessionRefreshBucket];
		foreach(KeepAliveRegistration *r, bucket)
		{
			// move to the end
			QPair<qint64, KeepAliveRegistration*> k(r->lastRefresh, r);
			sessionsByLastRefresh.remove(k);
			r->lastRefresh = now;
			sessionsByLastRefresh.insert(QPair<qint64, KeepAliveRegistration*>(r->lastRefresh, r), r);

			QByteArray peer = r->s->peer();
			if(peer.isEmpty())
				continue;

			if(!packets.contains(peer))
			{
				WsControlPacket packet;
				packet.from = identity;
				packets.insert(peer, packet);
			}

			WsControlPacket &packet = packets[peer];

			WsControlPacket::Item i;
			i.cid = r->s->cid();
			i.type = WsControlPacket::Item::KeepAlive;
			i.ttl = SESSION_EXPIRE / 1000;
			packet.items += i;

			// if we're at max, send out now
			if(packet.items.count() >= PACKET_ITEMS_MAX)
			{
				writeStream(packet, peer);
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

			QByteArray peer = r->s->peer();
			if(peer.isEmpty())
				continue;

			if(!packets.contains(peer))
			{
				WsControlPacket packet;
				packet.from = identity;
				packets.insert(peer, packet);
			}

			WsControlPacket &packet = packets[peer];

			WsControlPacket::Item i;
			i.cid = r->s->cid();
			i.type = WsControlPacket::Item::KeepAlive;
			i.ttl = SESSION_EXPIRE / 1000;
			packet.items += i;

			// if we're at max, send out now
			if(packet.items.count() >= PACKET_ITEMS_MAX)
			{
				writeStream(packet, peer);
				packet.items.clear();
			}
		}

		// send the rest
		QHashIterator<QByteArray, WsControlPacket> it(packets);
		while(it.hasNext())
		{
			it.next();
			const QByteArray &peer = it.key();
			const WsControlPacket &packet = it.value();

			if(!packet.items.isEmpty())
				writeStream(packet, peer);
		}

		++currentSessionRefreshBucket;
		if(currentSessionRefreshBucket >= SESSION_REFRESH_BUCKETS)
			currentSessionRefreshBucket = 0;
	}
};

WsControlManager::WsControlManager() 
{
	d = std::make_shared<Private>(this);
}

WsControlManager::~WsControlManager() = default;

void WsControlManager::setIdentity(const QByteArray &id)
{
	d->identity = id;
}

void WsControlManager::setIpcFileMode(int mode)
{
	d->ipcFileMode = mode;
}

bool WsControlManager::setInitSpecs(const QStringList &specs)
{
	d->initSpecs = specs;
	return d->setupInit();
}

bool WsControlManager::setStreamSpecs(const QStringList &specs)
{
	d->streamSpecs = specs;
	return d->setupStream();
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

void WsControlManager::writeInit(const WsControlPacket::Item &item)
{
	d->writeInit(item);
}

void WsControlManager::writeStream(const WsControlPacket::Item &item, const QByteArray &instanceAddress)
{
	d->writeStream(item, instanceAddress);
}

void WsControlManager::registerKeepAlive(WsControlSession *s)
{
	d->registerKeepAlive(s);
}

void WsControlManager::unregisterKeepAlive(WsControlSession *s)
{
	d->unregisterKeepAlive(s);
}
