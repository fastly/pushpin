/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifndef ZHTTPREQUEST_H
#define ZHTTPREQUEST_H

#include <QObject>
#include <QUrl>
#include <QHostAddress>
#include <QVariant>
#include "httpheaders.h"

class ZhttpRequestPacket;
class ZhttpResponsePacket;
class ZhttpManager;

class ZhttpRequest : public QObject
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

	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	class ServerState
	{
	public:
		Rid rid;
		QHostAddress peerAddress;
		QString requestMethod;
		QUrl requestUri;
		HttpHeaders requestHeaders;
		QByteArray requestBody;
		int inSeq;
		int outSeq;
		int outCredits;
		QVariant userData;

		ServerState() :
			inSeq(-1),
			outSeq(-1),
			outCredits(-1)
		{
		}
	};

	~ZhttpRequest();

	Rid rid() const;

	QHostAddress peerAddress() const;

	void setConnectHost(const QString &host);
	void setConnectPort(int port);
	void setIgnorePolicies(bool on);
	void setIgnoreTlsErrors(bool on);
	void setIsTls(bool on); // updates scheme

	void start(const QString &method, const QUrl &uri, const HttpHeaders &headers);
	void beginResponse(int code, const QByteArray &reason, const HttpHeaders &headers);

	// may call this multiple times
	void writeBody(const QByteArray &body);

	void endBody();

	// for server requests only
	void pause();
	void resume();
	ServerState serverState() const;

	int bytesAvailable() const;
	int writeBytesAvailable() const;
	bool isFinished() const;
	bool isInputFinished() const;
	bool isOutputFinished() const;
	bool isErrored() const;
	ErrorCondition errorCondition() const;

	QString requestMethod() const;
	QUrl requestUri() const;
	HttpHeaders requestHeaders() const;

	int responseCode() const;
	QByteArray responseReason() const;
	HttpHeaders responseHeaders() const;

	QByteArray readBody(int size = -1); // takes from the buffer

signals:
	// indicates input data and/or input finished
	void readyRead();
	// indicates output data written and/or output finished
	void bytesWritten(int count);
	void paused();
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZhttpManager;
	ZhttpRequest(QObject *parent = 0);
	void setupClient(ZhttpManager *manager, bool req);
	bool setupServer(ZhttpManager *manager, const ZhttpRequestPacket &packet);
	void setupServer(ZhttpManager *manager, const ServerState &state);
	void startServer();
	bool isServer() const;
	void handle(const ZhttpRequestPacket &packet);
	void handle(const ZhttpResponsePacket &packet);
};

#endif
