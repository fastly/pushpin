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

#ifndef WSSESSION_H
#define WSSESSION_H

#include <QObject>
#include <QHash>
#include <QSet>
#include "packet/httprequestdata.h"

class QTimer;

class WsSession : public QObject
{
	Q_OBJECT

public:
	QString cid;
	int nextReqId;
	QString channelPrefix;
	HttpRequestData requestData;
	QString route;
	QString statsRoute;
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
	QTimer *expireTimer;
	QTimer *delayedTimer;
	QTimer *requestTimer;

	WsSession(QObject *parent = 0);
	~WsSession();

	void refreshExpiration();
	void flushDelayed();
	void sendDelayed(const QByteArray &type, const QByteArray &message, int timeout);
	void ack(int reqId);

signals:
	void send(int reqId, const QByteArray &type, const QByteArray &message);
	void expired();
	void error();

private:
	void setupRequestTimer();

private slots:
	void expireTimer_timeout();
	void delayedTimer_timeout();
	void requestTimer_timeout();
};

#endif
