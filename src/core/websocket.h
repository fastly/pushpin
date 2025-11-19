/*
 * Copyright (C) 2014 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
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

#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <QUrl>
#include <QHostAddress>
#include "httpheaders.h"
#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;

class WebSocket
{
public:
	enum State
	{
		Idle,
		Connecting,
		Connected,
		Closing
	};

	enum ErrorCondition
	{
		ErrorGeneric,
		ErrorPolicy,
		ErrorConnect,
		ErrorConnectTimeout,
		ErrorTls,
		ErrorRejected,
		ErrorTimeout,
		ErrorUnavailable
	};

	class Frame
	{
	public:
		enum Type
		{
			Continuation,
			Text,
			Binary,
			Ping,
			Pong
		};

		Type type;
		QByteArray data;
		bool more;

		Frame(Type _type, const QByteArray &_data, bool _more) :
			type(_type),
			data(_data),
			more(_more)
		{
		}
	};

	virtual ~WebSocket() = default;

	virtual QHostAddress peerAddress() const = 0;

	virtual void setConnectHost(const QString &host) = 0;
	virtual void setConnectPort(int port) = 0;
	virtual void setIgnorePolicies(bool on) = 0;
	virtual void setTrustConnectHost(bool on) = 0;
	virtual void setIgnoreTlsErrors(bool on) = 0;
	virtual void setClientCert(const QString &cert, const QString &key) = 0;

	virtual void start(const QUrl &uri, const HttpHeaders &headers) = 0;

	virtual void respondSuccess(const QByteArray &reason, const HttpHeaders &headers) = 0;
	virtual void respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body) = 0;

	virtual State state() const = 0;
	virtual QUrl requestUri() const = 0;
	virtual HttpHeaders requestHeaders() const = 0;
	virtual int responseCode() const = 0;
	virtual QByteArray responseReason() const = 0;
	virtual HttpHeaders responseHeaders() const = 0;
	virtual QByteArray responseBody() const = 0;
	virtual int framesAvailable() const = 0;
	virtual int writeBytesAvailable() const = 0;
	virtual int peerCloseCode() const = 0;
	virtual QString peerCloseReason() const = 0;
	virtual ErrorCondition errorCondition() const = 0;

	virtual void writeFrame(const Frame &frame) = 0;
	virtual Frame readFrame() = 0;
	virtual void close(int code = -1, const QString &reason = QString()) = 0;

	Signal connected;
	Signal readyRead;
	boost::signals2::signal<void(int, int)> framesWritten;
	Signal writeBytesChanged;
	Signal peerClosed; // Emitted only if peer closes before we do
	Signal closed; // Emitted after peer acks our close, or immediately if we were acking
	Signal error;
};

#endif
