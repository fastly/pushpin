/*
 * Copyright (C) 2016 Fanout, Inc.
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

#ifndef TESTHTTPREQUEST_H
#define TESTHTTPREQUEST_H

#include "httprequest.h"

class TestHttpRequest : public HttpRequest
{
	Q_OBJECT

public:
	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	TestHttpRequest(QObject *parent = 0);
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
	Private *d;
};

#endif
