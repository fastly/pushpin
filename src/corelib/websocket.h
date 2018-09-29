/*
 * Copyright (C) 2014 Fanout, Inc.
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

#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <QObject>
#include <QUrl>
#include <QHostAddress>
#include "httpheaders.h"

class WebSocket : public QObject
{
	Q_OBJECT

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

	WebSocket(QObject *parent = 0) : QObject(parent) {}

	virtual QHostAddress peerAddress() const = 0;

	virtual void setConnectHost(const QString &host) = 0;
	virtual void setConnectPort(int port) = 0;
	virtual void setIgnorePolicies(bool on) = 0;
	virtual void setTrustConnectHost(bool on) = 0;
	virtual void setIgnoreTlsErrors(bool on) = 0;

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
	virtual bool canWrite() const = 0;
	virtual int writeBytesAvailable() const = 0;
	virtual int peerCloseCode() const = 0;
	virtual QString peerCloseReason() const = 0;
	virtual ErrorCondition errorCondition() const = 0;

	virtual void writeFrame(const Frame &frame) = 0;
	virtual Frame readFrame() = 0;
	virtual void close(int code = -1, const QString &reason = QString()) = 0;

signals:
	void connected();
	void readyRead();
	void framesWritten(int count, int contentBytes);
	void peerClosed(); // emitted only if peer closes before we do
	void closed(); // emitted after peer acks our close, or immediately if we were acking
	void error();
};

#endif
