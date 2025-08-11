/*
 * Copyright (C) 2014-2022 Fanout, Inc.
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

#ifndef WSCONTROLSESSION_H
#define WSCONTROLSESSION_H

#include <QByteArray>
#include <boost/signals2.hpp>
#include "websocket.h"
#include "wscontrol.h"
#include "packet/wscontrolpacket.h"

using Signal = boost::signals2::signal<void()>;

class WsControlManager;

class WsControlSession
{
public:
	~WsControlSession();

	QByteArray peer() const;
	QByteArray cid() const;

	void start(bool debug, const QByteArray &routeId, bool separateStats, const QByteArray &channelPrefix, int logLevel, const QUrl &uri, bool targetTrusted);
	void sendGripMessage(const QByteArray &message);
	void sendNeedKeepAlive();
	void sendSubscribe(const QByteArray &channel);

	// tell session that a received sendEvent has been written
	void sendEventWritten();

	boost::signals2::signal<void(WebSocket::Frame::Type, const QByteArray&, bool)> sendEventReceived;
	boost::signals2::signal<void(WsControl::KeepAliveMode, int)> keepAliveSetupEventReceived;
	Signal refreshEventReceived;
	boost::signals2::signal<void(int, const QByteArray&)> closeEventReceived; // Use -1 for no code
	Signal detachEventReceived;
	Signal cancelEventReceived;
	Signal error;

private:
	class Private;
	friend class Private;
	std::unique_ptr<Private> d;

	friend class WsControlManager;
	WsControlSession();
	void setup(WsControlManager *manager, const QByteArray &cid);
	void handle(const QByteArray &from, const WsControlPacket::Item &item);
};

#endif
