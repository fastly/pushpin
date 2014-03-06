/*
 * Copyright (C) 2014 Fanout, Inc.
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

#ifndef ZWEBSOCKET_H
#define ZWEBSOCKET_H

#include <QObject>
#include <QUrl>
#include <QHostAddress>
#include <QVariant>
#include "httpheaders.h"

class ZhttpRequestPacket;
class ZhttpResponsePacket;
class ZhttpManager;

class ZWebSocket : public QObject
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

	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	~ZWebSocket();

	Rid rid() const;

	QHostAddress peerAddress() const;

	void setConnectHost(const QString &host);
	void setConnectPort(int port);
	void setIgnorePolicies(bool on);
	void setIgnoreTlsErrors(bool on);

	void start(const QUrl &uri, const HttpHeaders &headers);

	void respondSuccess(const QByteArray &reason, const HttpHeaders &headers);
	void respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body);

	State state() const;
	QUrl requestUri() const;
	HttpHeaders requestHeaders() const;
	int responseCode() const;
	QByteArray responseReason() const;
	HttpHeaders responseHeaders() const;
	QByteArray responseBody() const;
	int framesAvailable() const;
	int peerCloseCode() const;
	ErrorCondition errorCondition() const;

	void writeFrame(const Frame &frame);
	Frame readFrame();
	void close(int code = -1);

signals:
	void connected();
	void readyRead();
	void framesWritten(int count);
	void peerClosed(); // emitted only if peer closes before we do
	void closed(); // emitted after peer acks our close, or immediately if we were acking
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZhttpManager;
	ZWebSocket(QObject *parent = 0);
	void setupClient(ZhttpManager *manager);
	bool setupServer(ZhttpManager *manager, const ZhttpRequestPacket &packet);
	void startServer();
	bool isServer() const;
	void handle(const ZhttpRequestPacket &packet);
	void handle(const ZhttpResponsePacket &packet);
};

#endif
