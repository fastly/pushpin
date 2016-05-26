/*
 * Copyright (C) 2015 Fanout, Inc.
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

#ifndef SIMPLEHTTPSERVER_H

#include <QObject>
#include <QHostAddress>

class HttpHeaders;

class SimpleHttpServerPrivate;

class SimpleHttpRequest : public QObject
{
	Q_OBJECT

public:
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
	SimpleHttpServer(QObject *parent = 0);
	~SimpleHttpServer();

	bool listen(const QHostAddress &addr, int port);
	SimpleHttpRequest *takeNext();

signals:
	void requestReady();

private:
	friend class SimpleHttpServerPrivate;
	SimpleHttpServerPrivate *d;
};

#endif
