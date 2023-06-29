/*
 * Copyright (C) 2014-2020 Fanout, Inc.
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

#ifndef WEBSOCKETOVERHTTP_H
#define WEBSOCKETOVERHTTP_H

#include "websocket.h"

class ZhttpManager;

class WebSocketOverHttp : public WebSocket
{
	Q_OBJECT

public:
	WebSocketOverHttp(ZhttpManager *zhttpManager, QObject *parent = 0);
	~WebSocketOverHttp();

	void setConnectionId(const QByteArray &id);
	void setMaxEventsPerRequest(int max);
	void refresh();

	static void setMaxManagedDisconnects(int max);
	static void clearDisconnectManager();

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
	virtual int peerCloseCode() const;
	virtual QString peerCloseReason() const;
	virtual ErrorCondition errorCondition() const;

	virtual void writeFrame(const Frame &frame);
	virtual Frame readFrame();
	virtual void close(int code = -1, const QString &reason = QString());

	void setHeaders(const HttpHeaders &headers);

signals:
	void aboutToSendRequest();
	void disconnected();

private:
	class DisconnectManager;
	friend class DisconnectManager;

	WebSocketOverHttp(QObject *parent = 0);
	void sendDisconnect();

	class Private;
	friend class Private;
	Private *d;

	static DisconnectManager *g_disconnectManager;
	static int g_maxManagedDisconnects;
};

#endif
