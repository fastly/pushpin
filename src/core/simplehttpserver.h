/*
 * Copyright (C) 2015-2022 Fanout, Inc.
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

#ifndef SIMPLEHTTPSERVER_H
#define SIMPLEHTTPSERVER_H

#include <QHostAddress>
#include <boost/signals2.hpp>
#include <map>

#define SOCKETNOTIFIERS_PER_SIMPLEHTTPREQUEST 1

using std::map;
using Signal = boost::signals2::signal<void()>;
using Connection = boost::signals2::scoped_connection;

class HttpHeaders;

class SimpleHttpServerPrivate;

class SimpleHttpRequest
{
public:
	~SimpleHttpRequest();

	QString requestMethod() const;
	QByteArray requestUri() const;
	HttpHeaders requestHeaders() const;
	QByteArray requestBody() const;

	void respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body);
	void respond(int code, const QByteArray &reason, const QString &body);

	Signal finished;

private:
	class Private;
	friend class Private;
	friend class SimpleHttpServerPrivate;
	Private *d;

	SimpleHttpRequest(int headersSizeMax, int bodySizeMax);
};

class SimpleHttpServer
{
public:
	SimpleHttpServer(int connectionsMax, int headersSizeMax, int bodySizeMax);
	~SimpleHttpServer();

	bool listen(const QHostAddress &addr, int port);
	bool listenLocal(const QString &name);
	SimpleHttpRequest *takeNext();

	Signal requestReady;

private:
	friend class SimpleHttpServerPrivate;
	SimpleHttpServerPrivate *d;
};

#endif
