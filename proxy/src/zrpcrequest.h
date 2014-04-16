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

#ifndef ZRPCREQUEST_H
#define ZRPCREQUEST_H

#include <QObject>
#include <QVariant>

class ZrpcRequestPacket;
class ZrpcManager;

class ZrpcRequest : public QObject
{
	Q_OBJECT

public:
	~ZrpcRequest();

	QString method() const;
	QVariantHash args() const;

	void respond(const QVariant &value = QVariant());
	void respondError(const QByteArray &condition);

private:
	class Private;
	Private *d;

	friend class ZrpcManager;
	ZrpcRequest(QObject *parent = 0);
	void setup(ZrpcManager *manager);
	void handle(const ZrpcRequestPacket &packet);
};

#endif
