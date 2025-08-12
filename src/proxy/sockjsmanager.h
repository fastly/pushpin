/*
 * Copyright (C) 2015-2017 Fanout, Inc.
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

#ifndef SOCKJSMANAGER_H
#define SOCKJSMANAGER_H

#include "domainmap.h"
#include <boost/signals2.hpp>
#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;
using Connection = boost::signals2::scoped_connection;

class HttpHeaders;
class ZhttpRequest;
class ZWebSocket;
class SockJsSession;

class SockJsManager
{
public:
	SockJsManager(const QString &sockJsUrl);
	~SockJsManager();

	void giveRequest(ZhttpRequest *req, int basePathStart, const QByteArray &asPath = QByteArray(), const DomainMap::Entry &route = DomainMap::Entry());
	void giveSocket(ZWebSocket *sock, int basePathStart, const QByteArray &asPath = QByteArray(), const DomainMap::Entry &route = DomainMap::Entry());

	SockJsSession *takeNext();

	Signal sessionReady;

private:
	class Private;
	friend class Private;
	Private *d;

	friend class SockJsSession;
	void unlink(SockJsSession *sess);
	void setLinger(SockJsSession *sess, const QVariant &closeValue);
	void respondOk(ZhttpRequest *req, const QVariant &data, const QByteArray &prefix = QByteArray(), const QByteArray &jsonpCallback = QByteArray());
	void respondOk(ZhttpRequest *req, const QString &str, const QByteArray &jsonpCallback = QByteArray());
	void respondError(ZhttpRequest *req, int code, const QByteArray &reason, const QString &message, bool discard = false);
	void respond(ZhttpRequest *req, int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body);
};

#endif
