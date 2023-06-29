/*
 * Copyright (C) 2014-2022 Fanout, Inc.
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
#include <QTimer>
#include <QDateTime>
#include <QUrl>
#include "wscontrolmanager.h"

#define SESSION_TTL 60
#define REQUEST_TIMEOUT 8000

class WsControlSession::Private : public QObject
{
	Q_OBJECT

public:
	WsControlSession *q;
	WsControlManager *manager;
	int nextReqId;
	QHash<int, qint64> pendingRequests;
	QList<QByteArray> pendingSendEventWrites;
	QTimer *requestTimer;
	QByteArray cid;
	QByteArray route;
	bool separateStats;
	QByteArray channelPrefix;
	QUrl uri;

	Private(WsControlSession *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		nextReqId(0),
		separateStats(false)
	{
		requestTimer = new QTimer(this);
		requestTimer->setSingleShot(true);
		connect(requestTimer, &QTimer::timeout, this, &Private::requestTimer_timeout);
	}

	~Private()
	{
		cleanup();

		requestTimer->setParent(0);
		requestTimer->disconnect(this);
		requestTimer->deleteLater();
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

		WsControlPacket::Item i;
		i.type = WsControlPacket::Item::Here;
		i.route = route;
		i.separateStats = separateStats;
		i.channelPrefix = channelPrefix;
		i.uri = uri;
		i.ttl = SESSION_TTL;
		write(i);
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

	void write(const WsControlPacket::Item &item)
	{
		WsControlPacket::Item out = item;
		out.cid = cid;
		manager->write(out);
	}

	void handle(const WsControlPacket::Item &item)
	{
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

			emit q->sendEventReceived(type, item.message, item.queue);
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
				emit q->keepAliveSetupEventReceived(mode, item.timeout);
			}
			else
				emit q->keepAliveSetupEventReceived(WsControl::NoKeepAlive);
		}
		else if(item.type == WsControlPacket::Item::Close)
		{
			emit q->closeEventReceived(item.code, item.reason);
		}
		else if(item.type == WsControlPacket::Item::Detach)
		{
			emit q->detachEventReceived();
		}
		else if(item.type == WsControlPacket::Item::Cancel)
		{
			emit q->cancelEventReceived();
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

		emit q->error();
	}
};

WsControlSession::WsControlSession(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

WsControlSession::~WsControlSession()
{
	delete d;
}

QByteArray WsControlSession::cid() const
{
	return d->cid;
}

void WsControlSession::start(const QByteArray &routeId, bool separateStats, const QByteArray &channelPrefix, const QUrl &uri)
{
	d->route = routeId;
	d->separateStats = separateStats;
	d->channelPrefix = channelPrefix;
	d->uri = uri;
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

void WsControlSession::handle(const WsControlPacket::Item &item)
{
	assert(d->manager);

	d->handle(item);
}

#include "wscontrolsession.moc"
