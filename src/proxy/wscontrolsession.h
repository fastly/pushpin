/*
 * Copyright (C) 2014-2017 Fanout, Inc.
 *
 * This file is part of Pushpin.
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
 */

#ifndef WSCONTROLSESSION_H
#define WSCONTROLSESSION_H

#include <QByteArray>
#include <QObject>
#include "websocket.h"
#include "packet/wscontrolpacket.h"

class WsControlManager;

class WsControlSession : public QObject
{
	Q_OBJECT

public:
	~WsControlSession();

	QByteArray cid() const;

	void start(const QByteArray &routeId, const QByteArray &channelPrefix, const QUrl &uri);
	void sendGripMessage(const QByteArray &message);
	void sendNeedKeepAlive();
	void sendSubscribe(const QByteArray &channel);

	// tell session that a received sendEvent has been written
	void sendEventWritten();

signals:
	void sendEventReceived(WebSocket::Frame::Type type, const QByteArray &message, bool queue);
	void keepAliveSetupEventReceived(bool enable, int timeout = -1);
	void closeEventReceived(int code); // -1 for no code
	void detachEventReceived();
	void cancelEventReceived();
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class WsControlManager;
	WsControlSession(QObject *parent = 0);
	void setup(WsControlManager *manager, const QByteArray &cid);
	void handle(const WsControlPacket::Item &item);
};

#endif
