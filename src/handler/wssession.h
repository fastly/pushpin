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

#ifndef WSSESSION_H
#define WSSESSION_H

#include <QHash>
#include <QSet>
#include <boost/signals2.hpp>
#include "packet/httprequestdata.h"
#include "packet/wscontrolpacket.h"
#include "ratelimiter.h"
#include "filter.h"
#include "clientsession.h"

// each session can have a bunch of timers:
// 3 misc timers
// filter timers
#define TIMERS_PER_WSSESSION (3 + TIMERS_PER_MESSAGEFILTERSTACK)

using Signal = boost::signals2::signal<void()>;
using Connection = boost::signals2::scoped_connection;

class Timer;
class ZhttpManager;
class PublishItem;

class WsSession : public ClientSession
{
public:
	QByteArray peer;
	QString cid;
	int nextReqId;
	bool debug;
	QString channelPrefix;
	int logLevel;
	HttpRequestData requestData;
	QString route;
	QString statsRoute;
	bool targetTrusted;
	QString sid;
	QHash<QString, QString> meta;
	QHash<QString, QStringList> channelFilters; // k=channel, v=list(filters)
	QSet<QString> channels;
	QSet<QString> implicitChannels;
	int ttl;
	QByteArray keepAliveType;
	QByteArray keepAliveMessage;
	QByteArray delayedType;
	QByteArray delayedMessage;
	QHash<int, qint64> pendingRequests;
	std::unique_ptr<Timer> expireTimer;
	std::unique_ptr<Timer> delayedTimer;
	std::unique_ptr<Timer> requestTimer;
	QList<PublishItem> publishQueue;
	ZhttpManager *zhttpOut;
	std::shared_ptr<RateLimiter> filterLimiter;
	std::unique_ptr<Filter::MessageFilter> filters;
	Connection filtersFinishedConnection;
	bool inProcessPublishQueue;
	bool closed;

	WsSession();
	~WsSession();

	void refreshExpiration();
	void flushDelayed();
	void sendDelayed(const QByteArray &type, const QByteArray &message, int timeout);
	void ack(int reqId);
	void publish(const PublishItem &item);
	void sendCloseError(const QString &message);

	boost::signals2::signal<void(const WsControlPacket::Item&)> send;
	Signal expired;
	Signal error;

private:
	void processPublishQueue();
	void filtersFinished(const Filter::MessageFilter::Result &result);
	void afterFilters(const PublishItem &item, Filter::SendAction sendAction, const QByteArray &content);
	void setupRequestTimer();
	void expireTimer_timeout();
	void delayedTimer_timeout();
	void requestTimer_timeout();
};

#endif
