/*
 * Copyright (C) 2016 Fanout, Inc.
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

#ifndef HTTPSESSIONUPDATEMANAGER_H
#define HTTPSESSIONUPDATEMANAGER_H

#include <QObject>

class QUrl;
class HttpSession;

class HttpSessionUpdateManager : public QObject
{
public:
	HttpSessionUpdateManager(QObject *parent = 0);
	~HttpSessionUpdateManager();

	void registerSession(HttpSession *hs, int timeout, const QUrl &uri);
	void unregisterSession(HttpSession *hs);

private:
	class Private;
	Private *d;
};

#endif
