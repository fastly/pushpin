/*
 * Copyright (C) 2012-2016 Fanout, Inc.
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

#ifndef HTTPREQUEST_H
#define HTTPREQUEST_H

#include <QObject>
#include <QUrl>
#include <QHostAddress>
#include "httpheaders.h"

class HttpRequest : public QObject
{
	Q_OBJECT

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

	HttpRequest(QObject *parent = 0) : QObject(parent) {}

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

signals:
	// indicates input data and/or input finished
	void readyRead();
	// indicates output data written and/or output finished
	void bytesWritten(int count);
	void paused();
	void error();
};

#endif
