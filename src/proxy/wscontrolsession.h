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

#ifndef WSCONTROLSESSION_H
#define WSCONTROLSESSION_H

#include <QByteArray>
#include <QObject>
#include "websocket.h"
#include "wscontrol.h"
#include "packet/wscontrolpacket.h"

class WsControlManager;

class WsControlSession : public QObject
{
	Q_OBJECT

public:
	~WsControlSession();

	QByteArray cid() const;

	void start(const QByteArray &routeId, bool separateStats, const QByteArray &channelPrefix, const QUrl &uri);
	void sendGripMessage(const QByteArray &message);
	void sendNeedKeepAlive();
	void sendSubscribe(const QByteArray &channel);

	// tell session that a received sendEvent has been written
	void sendEventWritten();

signals:
	void sendEventReceived(WebSocket::Frame::Type type, const QByteArray &message, bool queue);
	void keepAliveSetupEventReceived(WsControl::KeepAliveMode mode, int timeout = -1);
	void closeEventReceived(int code, const QByteArray &reason); // -1 for no code
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
