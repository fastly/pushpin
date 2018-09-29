/*
 * Copyright (C) 2015 Fanout, Inc.
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

#ifndef SOCKJSSESSION_H
#define SOCKJSSESSION_H

#include <QObject>
#include <QUrl>
#include <QHostAddress>
#include "httpheaders.h"
#include "websocket.h"
#include "domainmap.h"

class ZhttpRequest;
class ZWebSocket;
class SockJsManager;

class SockJsSession : public WebSocket
{
	Q_OBJECT

public:
	~SockJsSession();

	QByteArray sid() const;
	DomainMap::Entry route() const;

	// reimplemented

	virtual QHostAddress peerAddress() const;

	virtual void setConnectHost(const QString &host);
	virtual void setConnectPort(int port);
	virtual void setIgnorePolicies(bool on);
	virtual void setTrustConnectHost(bool on);
	virtual void setIgnoreTlsErrors(bool on);

	virtual void start(const QUrl &uri, const HttpHeaders &headers);

	virtual void respondSuccess(const QByteArray &reason, const HttpHeaders &headers);
	virtual void respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body);

	virtual State state() const;
	virtual QUrl requestUri() const;
	virtual HttpHeaders requestHeaders() const;
	virtual int responseCode() const;
	virtual QByteArray responseReason() const;
	virtual HttpHeaders responseHeaders() const;
	virtual QByteArray responseBody() const;
	virtual int framesAvailable() const;
	virtual bool canWrite() const;
	virtual int writeBytesAvailable() const;
	virtual int peerCloseCode() const;
	virtual QString peerCloseReason() const;
	virtual ErrorCondition errorCondition() const;

	virtual void writeFrame(const Frame &frame);
	virtual Frame readFrame();
	virtual void close(int code = -1, const QString &reason = QString());

private:
	class Private;
	friend class Private;
	Private *d;

	friend class SockJsManager;
	SockJsSession(QObject *parent = 0);
	void setupServer(SockJsManager *manager, ZhttpRequest *req, const QByteArray &jsonpCallback, const QUrl &asUri, const QByteArray &sid, const QByteArray &lastPart, const QByteArray &body, const DomainMap::Entry &route);
	void setupServer(SockJsManager *manager, ZWebSocket *sock, const QUrl &asUri, const DomainMap::Entry &route);
	void setupServer(SockJsManager *manager, ZWebSocket *sock, const QUrl &asUri, const QByteArray &sid, const QByteArray &lastPart, const DomainMap::Entry &route);

	void startServer();
	void handleRequest(ZhttpRequest *req, const QByteArray &jsonpCallback, const QByteArray &lastPart, const QByteArray &body);
};

#endif
