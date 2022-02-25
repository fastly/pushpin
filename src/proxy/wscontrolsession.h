/*
 * Copyright (C) 2014-2022 Fanout, Inc.
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
