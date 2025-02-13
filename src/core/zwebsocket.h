/*
 * Copyright (C) 2014-2016 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#ifndef ZWEBSOCKET_H
#define ZWEBSOCKET_H

#include "websocket.h"
#include <boost/signals2.hpp>

using Connection = boost::signals2::scoped_connection;

class ZhttpRequestPacket;
class ZhttpResponsePacket;
class ZhttpManager;

class ZWebSocket : public WebSocket
{
	Q_OBJECT

public:
	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	~ZWebSocket();

	Rid rid() const;
	void setIsTls(bool on);

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

	friend class ZhttpManager;
	ZWebSocket(QObject *parent = 0);
	void setupClient(ZhttpManager *manager);
	bool setupServer(ZhttpManager *manager, const QByteArray &id, int seq, const ZhttpRequestPacket &packet);
	void startServer();
	bool isServer() const;
	QByteArray toAddress() const;
	int outSeqInc();
	void handle(const QByteArray &id, int seq, const ZhttpRequestPacket &packet);
	void handle(const QByteArray &id, int seq, const ZhttpResponsePacket &packet);
};

#endif
