/*
 * Copyright (C) 2015-2022 Fanout, Inc.
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
