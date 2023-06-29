/*
 * Copyright (C) 2015-2022 Fanout, Inc.
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

#include <QObject>
#include <QHostAddress>

class HttpHeaders;

class SimpleHttpServerPrivate;

class SimpleHttpRequest : public QObject
{
	Q_OBJECT

public:
	SimpleHttpRequest(int maxHeadersSize, int maxBodySize, QObject* parent = 0);
	~SimpleHttpRequest();

	QString requestMethod() const;
	QByteArray requestUri() const;
	HttpHeaders requestHeaders() const;
	QByteArray requestBody() const;

	void respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body);
	void respond(int code, const QByteArray &reason, const QString &body);

signals:
	void finished();

private:
	class Private;
	friend class Private;
	friend class SimpleHttpServerPrivate;
	Private *d;

	SimpleHttpRequest(QObject *parent = 0);
};

class SimpleHttpServer : public QObject
{
	Q_OBJECT

public:
	SimpleHttpServer(int maxHeadersSize, int maxBodySize, QObject *parent = 0);
	~SimpleHttpServer();

	bool listen(const QHostAddress &addr, int port);
	bool listenLocal(const QString &name);
	SimpleHttpRequest *takeNext();

signals:
	void requestReady();

private:
	friend class SimpleHttpServerPrivate;
	SimpleHttpServerPrivate *d;
};

#endif
