/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
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

#ifndef ZURLMANAGER_H
#define ZURLMANAGER_H

#include <QObject>

class ZurlRequestPacket;
class ZurlRequest;

class ZurlManager : public QObject
{
	Q_OBJECT

public:
	ZurlManager(QObject *parent = 0);
	~ZurlManager();

	QByteArray clientId() const;

	void setClientId(const QByteArray &id);
	bool setOutgoingSpecs(const QStringList &specs);
	bool setOutgoingStreamSpecs(const QStringList &specs);
	bool setIncomingSpecs(const QStringList &specs);

	ZurlRequest *createRequest();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZurlRequest;
	void link(ZurlRequest *req);
	void unlink(ZurlRequest *req);
	void write(const ZurlRequestPacket &packet);
	void write(const ZurlRequestPacket &packet, const QByteArray &instanceAddress);
};

#endif
