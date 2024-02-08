/*
 * Copyright (C) 2012-2016 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
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

#ifndef HTTPREQUEST_H
#define HTTPREQUEST_H

#include <QUrl>
#include <QHostAddress>
#include "httpheaders.h"
#include <boost/signals2.hpp>
#include <memory>

using Signal = boost::signals2::signal<void()>;
using SignalInt = boost::signals2::signal<void(int)>;

class HttpRequest
{
public:
	enum ErrorCondition
	{
		ErrorGeneric,
		ErrorPolicy,
		ErrorConnect,
		ErrorConnectTimeout,
		ErrorTls,
		ErrorLengthRequired,
		ErrorDisconnected,
		ErrorTimeout,
		ErrorUnavailable,
		ErrorRequestTooLarge
	};

	HttpRequest() {}

	virtual QHostAddress peerAddress() const = 0;

	virtual void setConnectHost(const QString &host) = 0;
	virtual void setConnectPort(int port) = 0;
	virtual void setIgnorePolicies(bool on) = 0;
	virtual void setTrustConnectHost(bool on) = 0;
	virtual void setIgnoreTlsErrors(bool on) = 0;

	virtual void start(const QString &method, const QUrl &uri, const HttpHeaders &headers) = 0;
	virtual void beginResponse(int code, const QByteArray &reason, const HttpHeaders &headers) = 0;

	// may call this multiple times
	virtual void writeBody(const QByteArray &body) = 0;

	virtual void endBody() = 0;

	virtual int bytesAvailable() const = 0;
	virtual int writeBytesAvailable() const = 0;
	virtual bool isFinished() const = 0;
	virtual bool isInputFinished() const = 0;
	virtual bool isOutputFinished() const = 0;
	virtual bool isErrored() const = 0;
	virtual ErrorCondition errorCondition() const = 0;

	virtual QString requestMethod() const = 0;
	virtual QUrl requestUri() const = 0;
	virtual HttpHeaders requestHeaders() const = 0;

	virtual int responseCode() const = 0;
	virtual QByteArray responseReason() const = 0;
	virtual HttpHeaders responseHeaders() const = 0;

	virtual QByteArray readBody(int size = -1) = 0; // takes from the buffer

	// indicates input data and/or input finished
	Signal readyRead;
	// indicates output data written and/or output finished
	SignalInt bytesWritten;
	Signal writeBytesChanged;
	Signal paused;
	Signal error;
};

#endif
