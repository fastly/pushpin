/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#ifndef TESTHTTPREQUEST_H
#define TESTHTTPREQUEST_H

#include "httprequest.h"
#include <memory>

class TestHttpRequest : public HttpRequest
{
public:
	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	TestHttpRequest();
	~TestHttpRequest();

	// reimplemented

	virtual QHostAddress peerAddress() const;

	virtual void setConnectHost(const QString &host);
	virtual void setConnectPort(int port);
	virtual void setIgnorePolicies(bool on);
	virtual void setTrustConnectHost(bool on);
	virtual void setIgnoreTlsErrors(bool on);

	virtual void start(const QString &method, const QUrl &uri, const HttpHeaders &headers);
	virtual void beginResponse(int code, const QByteArray &reason, const HttpHeaders &headers);

	virtual void writeBody(const QByteArray &body);

	virtual void endBody();

	virtual int bytesAvailable() const;
	virtual int writeBytesAvailable() const;
	virtual bool isFinished() const;
	virtual bool isInputFinished() const;
	virtual bool isOutputFinished() const;
	virtual bool isErrored() const;
	virtual ErrorCondition errorCondition() const;

	virtual QString requestMethod() const;
	virtual QUrl requestUri() const;
	virtual HttpHeaders requestHeaders() const;

	virtual int responseCode() const;
	virtual QByteArray responseReason() const;
	virtual HttpHeaders responseHeaders() const;

	virtual QByteArray readBody(int size = -1);

private:
	class Private;
	friend class Private;
	std::unique_ptr<Private> d;
};

#endif
