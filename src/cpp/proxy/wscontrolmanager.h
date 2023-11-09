/*
 * Copyright (C) 2014 Fanout, Inc.
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

#ifndef WSCONTROLMANAGER_H
#define WSCONTROLMANAGER_H

#include <QObject>
#include "packet/wscontrolpacket.h"
#include <boost/signals2.hpp>

class WsControlSession;

class WsControlManager : public QObject
{
	Q_OBJECT

public:
	WsControlManager(QObject *parent = 0);
	~WsControlManager();

	void setIpcFileMode(int mode);

	bool setInSpec(const QString &spec);
	bool setOutSpec(const QString &spec);

	WsControlSession *createSession(const QByteArray &cid);

private:
	class Private;
	Private *d;

	friend class WsControlSession;
	void link(WsControlSession *s, const QByteArray &cid);
	void unlink(const QByteArray &cid);
	bool canWriteImmediately() const;
	void write(const WsControlPacket::Item &item);
	void registerKeepAlive(WsControlSession *s);
	void unregisterKeepAlive(WsControlSession *s);
};

#endif
