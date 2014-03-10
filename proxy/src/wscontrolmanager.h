/*
 * Copyright (C) 2014 Fanout, Inc.
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

#ifndef WSCONTROLMANAGER_H
#define WSCONTROLMANAGER_H

#include <QObject>
#include "packet/wscontrolpacket.h"

class WsControlSession;

class WsControlManager : public QObject
{
	Q_OBJECT

public:
	WsControlManager(QObject *parent = 0);
	~WsControlManager();

	bool setInSpec(const QString &spec);
	bool setOutSpec(const QString &spec);

	WsControlSession *createSession();

private:
	class Private;
	Private *d;

	friend class WsControlSession;
	void link(WsControlSession *s, const QByteArray &cid);
	void unlink(const QByteArray &cid);
	bool canWriteImmediately() const;
	void write(const WsControlPacket::Item &item);
};

#endif
