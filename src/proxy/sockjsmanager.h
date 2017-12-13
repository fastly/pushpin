/*
 * Copyright (C) 2015-2017 Fanout, Inc.
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

#ifndef SOCKJSMANAGER_H
#define SOCKJSMANAGER_H

#include <QObject>
#include "domainmap.h"

class HttpHeaders;
class ZhttpRequest;
class ZWebSocket;
class SockJsSession;

class SockJsManager : public QObject
{
	Q_OBJECT

public:
	SockJsManager(const QString &sockJsUrl, QObject *parent = 0);
	~SockJsManager();

	void giveRequest(ZhttpRequest *req, int basePathStart, const QByteArray &asPath = QByteArray(), const DomainMap::Entry &route = DomainMap::Entry());
	void giveSocket(ZWebSocket *sock, int basePathStart, const QByteArray &asPath = QByteArray(), const DomainMap::Entry &route = DomainMap::Entry());

	SockJsSession *takeNext();

signals:
	void sessionReady();

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
