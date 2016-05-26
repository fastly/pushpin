/*
 * Copyright (C) 2016 Fanout, Inc.
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

#ifndef TESTWEBSOCKET_H
#define TESTWEBSOCKET_H

#include "websocket.h"

class ZhttpManager;

class TestWebSocket : public WebSocket
{
	Q_OBJECT

public:
	TestWebSocket(QObject *parent = 0);
	~TestWebSocket();

	// reimplemented

	virtual QHostAddress peerAddress() const;

	virtual void setConnectHost(const QString &host);
	virtual void setConnectPort(int port);
	virtual void setIgnorePolicies(bool on);
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
	virtual ErrorCondition errorCondition() const;

	virtual void writeFrame(const Frame &frame);
	virtual Frame readFrame();
	virtual void close(int code = -1);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
