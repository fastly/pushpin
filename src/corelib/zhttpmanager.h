/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifndef ZHTTPMANAGER_H
#define ZHTTPMANAGER_H

#include <QObject>
#include "zhttprequest.h"
#include "zwebsocket.h"

class ZhttpRequestPacket;
class ZhttpResponsePacket;

class ZhttpManager : public QObject
{
	Q_OBJECT

public:
	ZhttpManager(QObject *parent = 0);
	~ZhttpManager();

	int connectionCount() const;
	bool clientUsesReq() const;
	ZhttpRequest *serverRequestByRid(const ZhttpRequest::Rid &rid) const;

	QByteArray instanceId() const;
	void setInstanceId(const QByteArray &id);

	void setIpcFileMode(int mode);
	void setBind(bool enable);

	bool setClientOutSpecs(const QStringList &specs);
	bool setClientOutStreamSpecs(const QStringList &specs);
	bool setClientInSpecs(const QStringList &specs);

	bool setClientReqSpecs(const QStringList &specs);

	bool setServerInSpecs(const QStringList &specs);
	bool setServerInStreamSpecs(const QStringList &specs);
	bool setServerOutSpecs(const QStringList &specs);

	ZhttpRequest *createRequest();
	ZhttpRequest *takeNextRequest();

	ZWebSocket *createSocket();
	ZWebSocket *takeNextSocket();

	// for server mode, jump directly to responding state
	ZhttpRequest *createRequestFromState(const ZhttpRequest::ServerState &state);

signals:
	void requestReady();
	void socketReady();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZhttpRequest;
	friend class ZWebSocket;
	void link(ZhttpRequest *req);
	void unlink(ZhttpRequest *req);
	void link(ZWebSocket *sock);
	void unlink(ZWebSocket *sock);
	bool canWriteImmediately() const;
	void writeHttp(const ZhttpRequestPacket &packet);
	void writeHttp(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress);
	void writeHttp(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress);
	void writeWs(const ZhttpRequestPacket &packet);
	void writeWs(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress);
	void writeWs(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress);

	void registerKeepAlive(ZhttpRequest *req);
	void unregisterKeepAlive(ZhttpRequest *req);
	void registerKeepAlive(ZWebSocket *sock);
	void unregisterKeepAlive(ZWebSocket *sock);
};

#endif
