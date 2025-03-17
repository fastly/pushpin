/*
 * Copyright (C) 2012-2013 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#ifndef ZHTTPMANAGER_H
#define ZHTTPMANAGER_H

#include <QObject>
#include "zhttprequest.h"
#include "zwebsocket.h"
#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;

class ZhttpRequestPacket;
class ZhttpResponsePacket;

class ZhttpManager : public QObject
{
	Q_OBJECT

public:
	ZhttpManager(QObject *parent = 0);
	~ZhttpManager();

	int connectionCount() const;
	bool clientUsesReq() const;
	ZhttpRequest *serverRequestByRid(const ZhttpRequest::Rid &rid) const;

	QByteArray instanceId() const;
	void setInstanceId(const QByteArray &id);

	void setIpcFileMode(int mode);
	void setBind(bool enable);

	bool setClientOutSpecs(const QStringList &specs);
	bool setClientOutStreamSpecs(const QStringList &specs);
	bool setClientInSpecs(const QStringList &specs);

	bool setClientReqSpecs(const QStringList &specs);

	bool setServerInSpecs(const QStringList &specs);
	bool setServerInStreamSpecs(const QStringList &specs);
	bool setServerOutSpecs(const QStringList &specs);

	void setCacheParameters(
		bool enable,
		const QStringList &httpBackendUrlList,
		const QStringList &wsBackendUrlList,
		const QStringList &cacheMethodList,
		const QStringList &subscribeMethodList,
		const QStringList &cacheKeyItemList,
		const QString &msgIdFieldName,
		const QString &msgMethodFieldName,
		const QString &msgParamsFieldName);
	
	int create_wsCacheClientProcesses();

	ZhttpRequest *createRequest();
	ZhttpRequest *takeNextRequest();

	ZWebSocket *createSocket();
	ZWebSocket *takeNextSocket();

	// for server mode, jump directly to responding state
	ZhttpRequest *createRequestFromState(const ZhttpRequest::ServerState &state);

	static int estimateRequestHeaderBytes(const QString &method, const QUrl &uri, const HttpHeaders &headers);
	static int estimateResponseHeaderBytes(int code, const QByteArray &reason, const HttpHeaders &headers);

	Signal requestReady;
	Signal socketReady;

private:
	class Private;
	friend class Private;
	std::shared_ptr<Private> d;

	friend class ZhttpRequest;
	friend class ZWebSocket;
	void link(ZhttpRequest *req);
	void unlink(ZhttpRequest *req);
	void link(ZWebSocket *sock);
	void unlink(ZWebSocket *sock);
	bool canWriteImmediately() const;
	void writeHttp(const ZhttpRequestPacket &packet);
	void writeHttp(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress);
	void writeHttp(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress);
	void writeWs(const ZhttpRequestPacket &packet);
	void writeWs(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress);
	void writeWs(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress);

	void registerKeepAlive(ZhttpRequest *req);
	void unregisterKeepAlive(ZhttpRequest *req);
	void registerKeepAlive(ZWebSocket *sock);
	void unregisterKeepAlive(ZWebSocket *sock);
};

#endif
