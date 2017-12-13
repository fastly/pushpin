/*
 * Copyright (C) 2014 Fanout, Inc.
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

#ifndef ZRPCMANAGER_H
#define ZRPCMANAGER_H

#include <QObject>

class ZrpcRequestPacket;
class ZrpcResponsePacket;
class ZrpcRequest;

class ZrpcManager : public QObject
{
	Q_OBJECT

public:
	ZrpcManager(QObject *parent = 0);
	~ZrpcManager();

	int timeout() const;

	void setIpcFileMode(int mode);
	void setBind(bool enable);
	void setTimeout(int ms);
	void setUnavailableOnTimeout(bool enable);

	bool setClientSpecs(const QStringList &specs);
	bool setServerSpecs(const QStringList &specs);

	ZrpcRequest *takeNext();

signals:
	void requestReady();

private:
	class Private;
	Private *d;

	friend class ZrpcRequest;
	bool canWriteImmediately() const;
	void link(ZrpcRequest *req);
	void unlink(ZrpcRequest *req);
	void write(const ZrpcRequestPacket &packet);
	void write(const QList<QByteArray> &headers, const ZrpcResponsePacket &packet);
};

#endif
