/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifndef ZHTTPMANAGER_H
#define ZHTTPMANAGER_H

#include <QObject>

class ZhttpRequestPacket;
class ZhttpRequest;

class ZhttpManager : public QObject
{
	Q_OBJECT

public:
	ZhttpManager(QObject *parent = 0);
	~ZhttpManager();

	QByteArray clientId() const;

	void setClientId(const QByteArray &id);
	bool setOutgoingSpecs(const QStringList &specs);
	bool setOutgoingStreamSpecs(const QStringList &specs);
	bool setIncomingSpecs(const QStringList &specs);

	ZhttpRequest *createRequest();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZhttpRequest;
	void link(ZhttpRequest *req);
	void unlink(ZhttpRequest *req);
	bool canWriteImmediately() const;
	void write(const ZhttpRequestPacket &packet);
	void write(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress);
};

#endif
