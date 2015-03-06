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

#ifndef SOCKJSMANAGER_H
#define SOCKJSMANAGER_H

#include <QObject>
#include "domainmap.h"

class HttpResponseData;
class ZhttpRequest;
class ZWebSocket;
class SockJsSession;

class SockJsManager : public QObject
{
	Q_OBJECT

public:
	SockJsManager(QObject *parent = 0);
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
	void link(SockJsSession *sess);
	void unlink(SockJsSession *sess);
	void respond(ZhttpRequest *req, const HttpResponseData &respData);
};

#endif
