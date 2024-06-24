/*
 * Copyright (C) 2012-2016 Fanout, Inc.
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

#ifndef ZHTTPREQUEST_H
#define ZHTTPREQUEST_H

#include <QVariant>
#include "httprequest.h"
#include <boost/signals2.hpp>

using Connection = boost::signals2::scoped_connection;

class ZhttpRequestPacket;
class ZhttpResponsePacket;
class ZhttpManager;

class ZhttpRequest : public HttpRequest
{
	Q_OBJECT

public:
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
		int responseCode;
		int inSeq;
		int outSeq;
		int outCredits;
		QVariant userData;

		ServerState() :
			responseCode(-1),
			inSeq(-1),
			outSeq(-1),
			outCredits(-1)
		{
		}
	};

	~ZhttpRequest();

	Rid rid() const;
	QVariant passthroughData() const;
	void setIsTls(bool on); // updates scheme
	void setSendBodyAfterAcknowledgement(bool on); // only works in push/sub mode
	void setPassthroughData(const QVariant &data);
	void setQuiet(bool on);

	// for server requests only
	void pause();
	void resume();
	ServerState serverState() const;

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

	friend class ZhttpManager;
	ZhttpRequest(QObject *parent = 0);
	void setupClient(ZhttpManager *manager, bool req);
	bool setupServer(ZhttpManager *manager, const QByteArray &id, int seq, const ZhttpRequestPacket &packet);
	void setupServer(ZhttpManager *manager, const ServerState &state);
	void startServer();
	bool isServer() const;
	QByteArray toAddress() const;
	int outSeqInc();
	void handle(const QByteArray &id, int seq, const ZhttpRequestPacket &packet);
	void handle(const QByteArray &id, int seq, const ZhttpResponsePacket &packet);
};

#endif
