/*
 * Copyright (C) 2020 Fanout, Inc.
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

#include "wssession.h"

#include <QDateTime>
#include "log.h"
#include "timer.h"
#include "defercall.h"
#include "filter.h"
#include "publishitem.h"
#include "publishformat.h"

#define WSCONTROL_REQUEST_TIMEOUT 8000

WsSession::WsSession() :
	nextReqId(0),
	debug(false),
	logLevel(LOG_LEVEL_DEBUG),
	targetTrusted(false),
	ttl(0),
	inProcessPublishQueue(false),
	closed(false)
{
	expireTimer = std::make_unique<Timer>();
	expireTimer->setSingleShot(true);
	expireTimer->timeout.connect(boost::bind(&WsSession::expireTimer_timeout, this));

	delayedTimer = std::make_unique<Timer>();
	delayedTimer->setSingleShot(true);
	delayedTimer->timeout.connect(boost::bind(&WsSession::delayedTimer_timeout, this));

	requestTimer = std::make_unique<Timer>();
	requestTimer->setSingleShot(true);
	requestTimer->timeout.connect(boost::bind(&WsSession::requestTimer_timeout, this));
}

WsSession::~WsSession() = default;

void WsSession::refreshExpiration()
{
	expireTimer->start(ttl * 1000);
}

void WsSession::flushDelayed()
{
	if(delayedTimer->isActive())
	{
		delayedTimer->stop();
		delayedTimer_timeout();
	}
}

void WsSession::sendDelayed(const QByteArray &type, const QByteArray &message, int timeout)
{
	flushDelayed();

	delayedType = type;
	delayedMessage = message;
	delayedTimer->start(timeout * 1000);
}

void WsSession::ack(int reqId)
{
	if(pendingRequests.contains(reqId))
	{
		pendingRequests.remove(reqId);
		setupRequestTimer();
	}
}

void WsSession::publish(const PublishItem &item)
{
	const PublishFormat &f = item.format;

	if(f.type != PublishFormat::WebSocketMessage)
		return;

	publishQueue += item;

	if(!inProcessPublishQueue)
		processPublishQueue();
}

void WsSession::processPublishQueue()
{
	assert(!inProcessPublishQueue);
	inProcessPublishQueue = true;

	while(!closed && !publishQueue.isEmpty() && !filters)
	{
		const PublishItem &item = publishQueue.first();
		const PublishFormat &f = item.format;

		if(f.haveContentFilters)
		{
			// ensure content filters match
			QStringList contentFilters;
			foreach(const QString &f, channelFilters[item.channel])
			{
				if(Filter::targets(f) & Filter::MessageContent)
					contentFilters += f;
			}
			if(contentFilters != f.contentFilters)
			{
				publishQueue.removeFirst();

				if(debug)
				{
					QString errorMessage = QString("content filter mismatch: subscription=%1 message=%2").arg(contentFilters.join(","), f.contentFilters.join(","));
					sendCloseError(errorMessage);
					break;
				}

				continue;
			}
		}

		filters = std::make_unique<Filter::MessageFilterStack>(channelFilters[item.channel]);
		filtersFinishedConnection = filters->finished.connect(boost::bind(&WsSession::filtersFinished, this, boost::placeholders::_1));

		// websocket sessions currently don't support previous IDs on
		// subscriptions, but we still need to populate the channel names in
		// in the filter context even if all the values will be null
		QHash<QString, QString> prevIds;
		foreach(const QString &name, channels)
			prevIds[name] = QString();

		Filter::Context fc;
		fc.prevIds = prevIds;
		fc.subscriptionMeta = meta;
		fc.publishMeta = item.meta;
		fc.zhttpOut = zhttpOut;
		fc.currentUri = requestData.uri;
		fc.route = route;
		fc.trusted = targetTrusted;
		fc.limiter = filterLimiter;

		// may call filtersFinished immediately. if it does, queue processing
		// will continue. else, the loop will end and queue processing will
		// resume after the filters finish
		filters->start(fc, f.body);
	}

	inProcessPublishQueue = false;
}

void WsSession::filtersFinished(const Filter::MessageFilter::Result &result)
{
	PublishItem item = publishQueue.takeFirst();

	filtersFinishedConnection.disconnect();
	filters.reset();

	if(!result.errorMessage.isNull())
	{
		if(debug)
		{
			QString errorMessage = QString("filter error: %1").arg(result.errorMessage);
			sendCloseError(errorMessage);
			return;
		}
	}
	else
	{
		afterFilters(item, result.sendAction, result.content);
	}

	// if filters finished asynchronously then we need to resume processing
	if(!inProcessPublishQueue)
		processPublishQueue();
}

void WsSession::afterFilters(const PublishItem &item, Filter::SendAction sendAction, const QByteArray &content)
{
	if(sendAction == Filter::Drop)
		return;

	const PublishFormat &f = item.format;

	// TODO: hint support for websockets?
	if(f.action != PublishFormat::Send && f.action != PublishFormat::Close && f.action != PublishFormat::Refresh)
		return;

	WsControlPacket::Item i;
	i.cid = cid.toUtf8();

	if(f.action == PublishFormat::Send)
	{
		i.type = WsControlPacket::Item::Send;

		switch(f.messageType)
		{
			case PublishFormat::Text:   i.contentType = "text"; break;
			case PublishFormat::Binary: i.contentType = "binary"; break;
			case PublishFormat::Ping:   i.contentType = "ping"; break;
			case PublishFormat::Pong:   i.contentType = "pong"; break;
			default: return; // unrecognized type, skip
		}

		i.message = content;
	}
	else if(f.action == PublishFormat::Close)
	{
		closed = true;

		i.type = WsControlPacket::Item::Close;
		i.code = f.code;
		i.reason = f.reason;
	}
	else if(f.action == PublishFormat::Refresh)
	{
		i.type = WsControlPacket::Item::Refresh;
	}

	send(i);
}

void WsSession::sendCloseError(const QString &message)
{
	closed = true;

	WsControlPacket::Item i;
	i.cid = cid.toUtf8();
	i.type = WsControlPacket::Item::Close;
	i.code = 1011;

	if(debug)
		i.reason = message.toUtf8();

	send(i);
}

void WsSession::setupRequestTimer()
{
	if(!pendingRequests.isEmpty())
	{
		// find next expiring request
		int64_t lowestTime = -1;
		QHashIterator<int, int64_t> it(pendingRequests);
		while(it.hasNext())
		{
			it.next();
			int64_t time = it.value();

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

void WsSession::expireTimer_timeout()
{
	log_debug("timing out ws session: %s", qPrintable(cid));

	expired();
}

void WsSession::delayedTimer_timeout()
{
	int reqId = nextReqId++;

	QByteArray message = delayedMessage;
	delayedMessage.clear();

	pendingRequests[reqId] = QDateTime::currentMSecsSinceEpoch() + WSCONTROL_REQUEST_TIMEOUT;
	setupRequestTimer();

	WsControlPacket::Item i;
	i.cid = cid.toUtf8();
	i.requestId = QByteArray::number(reqId);
	i.type = WsControlPacket::Item::Send;
	i.contentType = delayedType;
	i.message = message;
	i.queue = true;

	send(i);
}

void WsSession::requestTimer_timeout()
{
	// on error, destroy any other pending requests
	pendingRequests.clear();
	setupRequestTimer();

	error();
}
