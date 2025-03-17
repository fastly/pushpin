/*
 * Copyright (C) 2015 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
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

#ifndef SOCKJSSESSION_H
#define SOCKJSSESSION_H

#include <QObject>
#include <QUrl>
#include <QHostAddress>
#include "httpheaders.h"
#include "websocket.h"
#include "domainmap.h"
#include <boost/signals2.hpp>

using Connection = boost::signals2::scoped_connection;

class ZhttpRequest;
class ZWebSocket;
class SockJsManager;

class SockJsSession : public WebSocket
{
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
	std::shared_ptr<Private> d;

	friend class SockJsManager;
	SockJsSession();
	void setupServer(SockJsManager *manager, ZhttpRequest *req, const QByteArray &jsonpCallback, const QUrl &asUri, const QByteArray &sid, const QByteArray &lastPart, const QByteArray &body, const DomainMap::Entry &route);
	void setupServer(SockJsManager *manager, ZWebSocket *sock, const QUrl &asUri, const DomainMap::Entry &route);
	void setupServer(SockJsManager *manager, ZWebSocket *sock, const QUrl &asUri, const QByteArray &sid, const QByteArray &lastPart, const DomainMap::Entry &route);

	void startServer();
	void handleRequest(ZhttpRequest *req, const QByteArray &jsonpCallback, const QByteArray &lastPart, const QByteArray &body);
};

#endif
