/*
 * Copyright (C) 2020 Fanout, Inc.
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
