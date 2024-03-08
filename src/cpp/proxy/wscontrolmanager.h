/*
 * Copyright (C) 2014 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include <memory>
#include <QObject>
#include "packet/wscontrolpacket.h"

class WsControlSession;

class WsControlManager : public QObject
{
	Q_OBJECT

public:
	WsControlManager();
	~WsControlManager();

	void setIdentity(const QByteArray &id);
	void setIpcFileMode(int mode);

	bool setInitSpecs(const QStringList &specs);
	bool setStreamSpecs(const QStringList &specs);

	WsControlSession *createSession(const QByteArray &cid);

private:
	class Private;
	std::unique_ptr<Private> d;

	friend class WsControlSession;
	void link(WsControlSession *s, const QByteArray &cid);
	void unlink(const QByteArray &cid);
	void writeInit(const WsControlPacket::Item &item);
	void writeStream(const WsControlPacket::Item &item, const QByteArray &instanceAddress);
	void registerKeepAlive(WsControlSession *s);
	void unregisterKeepAlive(WsControlSession *s);
};

#endif
