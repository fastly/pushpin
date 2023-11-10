/*
 * Copyright (C) 2020 Fanout, Inc.
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

#include <QTimer>
#include <QDateTime>
#include "log.h"

#define WSCONTROL_REQUEST_TIMEOUT 8000

WsSession::WsSession(QObject *parent) :
	QObject(parent),
	nextReqId(0)
{
	expireTimer = new QTimer(this);
	expireTimer->setSingleShot(true);
	connect(expireTimer, &QTimer::timeout, this, &WsSession::expireTimer_timeout);

	delayedTimer = new QTimer(this);
	delayedTimer->setSingleShot(true);
	connect(delayedTimer, &QTimer::timeout, this, &WsSession::delayedTimer_timeout);

	requestTimer = new QTimer(this);
	requestTimer->setSingleShot(true);
	connect(requestTimer, &QTimer::timeout, this, &WsSession::requestTimer_timeout);
}

WsSession::~WsSession()
{
	expireTimer->disconnect(this);
	expireTimer->setParent(0);
	expireTimer->deleteLater();

	delayedTimer->disconnect(this);
	delayedTimer->setParent(0);
	delayedTimer->deleteLater();

	requestTimer->disconnect(this);
	requestTimer->setParent(0);
	requestTimer->deleteLater();
}

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

void WsSession::setupRequestTimer()
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

void WsSession::expireTimer_timeout()
{
	log_debug("timing out ws session: %s", qPrintable(cid));

	emit expired();
}

void WsSession::delayedTimer_timeout()
{
	int reqId = nextReqId++;

	QByteArray message = delayedMessage;
	delayedMessage.clear();

	pendingRequests[reqId] = QDateTime::currentMSecsSinceEpoch() + WSCONTROL_REQUEST_TIMEOUT;
	setupRequestTimer();

	emit send(reqId, delayedType, message);
}

void WsSession::requestTimer_timeout()
{
	// on error, destroy any other pending requests
	pendingRequests.clear();
	setupRequestTimer();

	emit error();
}
