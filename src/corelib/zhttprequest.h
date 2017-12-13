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

#ifndef ZHTTPREQUEST_H
#define ZHTTPREQUEST_H

#include <QVariant>
#include "httprequest.h"

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
