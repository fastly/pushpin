/*
 * Copyright (C) 2014-2022 Fanout, Inc.
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

#ifndef WSCONTROLPACKET_H
#define WSCONTROLPACKET_H

#include <QByteArray>
#include <QList>
#include <QVariant>
#include <QUrl>

class WsControlPacket
{
public:
	class Item
	{
	public:
		enum Type
		{
			Here,
			KeepAlive,
			Gone,
			Grip,
			NeedKeepAlive,
			Subscribe,
			Cancel,
			Send,
			KeepAliveSetup,
			Close,
			Detach,
			Ack
		};

		QByteArray cid;
		Type type;
		QByteArray requestId;
		QUrl uri;
		QByteArray contentType;
		QByteArray message;
		bool queue;
		int code;
		QByteArray reason;
		QByteArray route;
		bool separateStats;
		QByteArray channelPrefix;
		QByteArray channel;
		int ttl;
		int timeout;
		QByteArray keepAliveMode;

		Item() :
			type((Type)-1),
			queue(false),
			code(-1),
			separateStats(false),
			ttl(-1),
			timeout(-1)
		{
		}
	};

	QList<Item> items;

	QVariant toVariant() const;
	bool fromVariant(const QVariant &in);
};

#endif
