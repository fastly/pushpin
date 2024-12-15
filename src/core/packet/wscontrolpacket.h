/*
 * Copyright (C) 2014-2022 Fanout, Inc.
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
			Refresh,
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
		int logLevel;
		bool trusted;
		QByteArray channel;
		int ttl;
		int timeout;
		QByteArray keepAliveMode;

		Item() :
			type((Type)-1),
			queue(false),
			code(-1),
			separateStats(false),
			logLevel(-1),
			trusted(false),
			ttl(-1),
			timeout(-1)
		{
		}
	};

	QByteArray from;
	QList<Item> items;

	QVariant toVariant() const;
	bool fromVariant(const QVariant &in);
};

#endif
