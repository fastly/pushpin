/*
 * Copyright (C) 2014-2022 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include "wscontrolsession.h"

#include <assert.h>
#include <QDateTime>
#include <QUrl>
#include <boost/signals2.hpp>
#include "rtimer.h"
#include "wscontrolmanager.h"

#define SESSION_TTL 60
#define REQUEST_TIMEOUT 8000

using Connection = boost::signals2::scoped_connection;

class WsControlSession::Private : public QObject
{
	Q_OBJECT

public:
	WsControlSession *q;
	WsControlManager *manager;
	int nextReqId;
	QList<WsControlPacket::Item> pendingItems;
	QHash<int, qint64> pendingRequests;
	QList<QByteArray> pendingSendEventWrites;
	std::unique_ptr<RTimer> requestTimer;
	QByteArray peer;
	QByteArray cid;
	bool debug;
	QByteArray route;
	bool separateStats;
	QByteArray channelPrefix;
	int logLevel;
	QUrl uri;
	bool targetTrusted;
	Connection requestTimerConnection;

	Private(WsControlSession *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		nextReqId(0),
		debug(false),
		separateStats(false),
		logLevel(-1),
		targetTrusted(false)
	{
		requestTimer = std::make_unique<RTimer>();
		requestTimerConnection = requestTimer->timeout.connect(boost::bind(&Private::requestTimer_timeout, this));
		requestTimer->setSingleShot(true);
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(manager)
		{
			manager->unregisterKeepAlive(q);

			WsControlPacket::Item i;
			i.type = WsControlPacket::Item::Gone;
			write(i);

			manager->unlink(cid);
			manager = 0;
		}
	}

	void start()
	{
		manager->registerKeepAlive(q);

		int reqId = nextReqId++;

		WsControlPacket::Item i;
		i.type = WsControlPacket::Item::Here;
		i.requestId = QByteArray::number(reqId);
		i.debug = debug;
		i.route = route;
		i.separateStats = separateStats;
		i.channelPrefix = channelPrefix;
		i.logLevel = logLevel;
		i.uri = uri;
		i.trusted = targetTrusted;
		i.ttl = SESSION_TTL;
		write(i, true);
	}

	void setupRequestTimer()
	{
		if(!pendingRequests.isEmpty())
		{
			// find next expiring request
			qint64 lowestTime = -1;
			QHashIterator<int, qint64> it(pendingRequests);
			while(it.hasNext())
			{
				it.next();
				qint64 time = it.value();

				if(lowestTime == -1 || time < lowestTime)
					lowestTime = time;
			}

			int until = int(lowestTime - QDateTime::currentMSecsSinceEpoch());

			requestTimer->start(qMax(until, 0));
		}
		else
		{
			requestTimer->stop();
		}
	}

	void sendGripMessage(const QByteArray &message)
	{
		int reqId = nextReqId++;

		WsControlPacket::Item i;
		i.type = WsControlPacket::Item::Grip;
		i.requestId = QByteArray::number(reqId);
		i.message = message;

		pendingRequests[reqId] = QDateTime::currentMSecsSinceEpoch() + REQUEST_TIMEOUT;
		setupRequestTimer();

		write(i);
	}

	void sendNeedKeepAlive()
	{
		WsControlPacket::Item i;
		i.type = WsControlPacket::Item::NeedKeepAlive;
		write(i);
	}

	void sendSubscribe(const QByteArray &channel)
	{
		int reqId = nextReqId++;

		WsControlPacket::Item i;
		i.type = WsControlPacket::Item::Subscribe;
		i.requestId = QByteArray::number(reqId);
		i.channel = channel;

		pendingRequests[reqId] = QDateTime::currentMSecsSinceEpoch() + REQUEST_TIMEOUT;
		setupRequestTimer();

		write(i);
	}

	void write(const WsControlPacket::Item &item, bool init = false)
	{
		if(init)
		{
			WsControlPacket::Item out = item;
			out.cid = cid;

			manager->writeInit(out);
		}
		else
		{
			if(!peer.isEmpty())
			{
				WsControlPacket::Item out = item;
				out.cid = cid;

				manager->writeStream(out, peer);
			}
			else
				pendingItems += item;
		}
	}

	void flushPending()
	{
		while(!pendingItems.isEmpty())
		{
			WsControlPacket::Item out = pendingItems.takeFirst();
			out.cid = cid;

			manager->writeStream(out, peer);
		}
	}

	void handle(const QByteArray &from, const WsControlPacket::Item &item)
	{
		peer = from;

		flushPending();

		if(item.type != WsControlPacket::Item::Ack && !item.requestId.isEmpty())
		{
			// ack non-sends immediately
			if(item.type != WsControlPacket::Item::Send)
			{
				WsControlPacket::Item i;
				i.type = WsControlPacket::Item::Ack;
				i.requestId = item.requestId;
				write(i);
			}
		}

		if(item.type == WsControlPacket::Item::Send)
		{
			WebSocket::Frame::Type type;
			if(item.contentType == "binary")
				type = WebSocket::Frame::Binary;
			else if(item.contentType == "ping")
				type = WebSocket::Frame::Ping;
			else if(item.contentType == "pong")
				type = WebSocket::Frame::Pong;
			else
				type = WebSocket::Frame::Text;

			// for sends, don't ack until written

			if(!item.requestId.isEmpty())
				pendingSendEventWrites += item.requestId;
			else
				pendingSendEventWrites += QByteArray(); // placeholder

			q->sendEventReceived(type, item.message, item.queue);
		}
		else if(item.type == WsControlPacket::Item::KeepAliveSetup)
		{
			if(item.timeout > 0)
			{
				WsControl::KeepAliveMode mode;
				if(item.keepAliveMode == "interval")
					mode = WsControl::Interval;
				else // idle
					mode = WsControl::Idle;
				q->keepAliveSetupEventReceived(mode, item.timeout);
			}
			else
				q->keepAliveSetupEventReceived(WsControl::NoKeepAlive, -1);
		}
		else if(item.type == WsControlPacket::Item::Refresh)
		{
			q->refreshEventReceived();
		}
		else if(item.type == WsControlPacket::Item::Close)
		{
			q->closeEventReceived(item.code, item.reason);
		}
		else if(item.type == WsControlPacket::Item::Detach)
		{
			q->detachEventReceived();
		}
		else if(item.type == WsControlPacket::Item::Cancel)
		{
			q->cancelEventReceived();
		}
		else if(item.type == WsControlPacket::Item::Ack)
		{
			int reqId = item.requestId.toInt();
			if(pendingRequests.contains(reqId))
			{
				pendingRequests.remove(reqId);
				setupRequestTimer();
			}
		}
	}

	void sendEventWritten()
	{
		assert(!pendingSendEventWrites.isEmpty());

		QByteArray requestId = pendingSendEventWrites.takeFirst();
		if(!requestId.isNull())
		{
			WsControlPacket::Item i;
			i.type = WsControlPacket::Item::Ack;
			i.requestId = requestId;
			write(i);
		}
	}

private slots:
	void requestTimer_timeout()
	{
		// on error, destroy any other pending requests
		pendingRequests.clear();
		setupRequestTimer();

		q->error();
	}
};

WsControlSession::WsControlSession()
{
	d = std::make_unique<Private>(this);
}

WsControlSession::~WsControlSession() = default;

QByteArray WsControlSession::peer() const
{
	return d->peer;
}

QByteArray WsControlSession::cid() const
{
	return d->cid;
}

void WsControlSession::start(bool debug, const QByteArray &routeId, bool separateStats, const QByteArray &channelPrefix, int logLevel, const QUrl &uri, bool targetTrusted)
{
	d->debug = debug;
	d->route = routeId;
	d->separateStats = separateStats;
	d->channelPrefix = channelPrefix;
	d->logLevel = logLevel;
	d->uri = uri;
	d->targetTrusted = targetTrusted;
	d->start();
}

void WsControlSession::sendGripMessage(const QByteArray &message)
{
	d->sendGripMessage(message);
}

void WsControlSession::sendNeedKeepAlive()
{
	d->sendNeedKeepAlive();
}

void WsControlSession::sendSubscribe(const QByteArray &channel)
{
	d->sendSubscribe(channel);
}

void WsControlSession::setup(WsControlManager *manager, const QByteArray &cid)
{
	d->manager = manager;
	d->cid = cid;
	d->manager->link(this, d->cid);
}

void WsControlSession::sendEventWritten()
{
	d->sendEventWritten();
}

void WsControlSession::handle(const QByteArray &from, const WsControlPacket::Item &item)
{
	assert(d->manager);

	d->handle(from, item);
}

#include "wscontrolsession.moc"
