/*
 * Copyright (C) 2016-2017 Fanout, Inc.
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

#ifndef WSCONTROLMESSAGE_H
#define WSCONTROLMESSAGE_H

#include <QString>
#include <QStringList>

class QVariant;

class WsControlMessage
{
public:
	enum Type
	{
		Subscribe,
		Unsubscribe,
		Detach,
		Session,
		SetMeta,
		KeepAlive,
		SendDelayed,
		FlushDelayed
	};

	enum MessageType
	{
		Text,
		Binary,
		Ping,
		Pong
	};

	Type type;
	QString channel;
	QStringList filters;
	QString sessionId;
	QString metaName;
	QString metaValue;
	MessageType messageType;
	QByteArray content;
	int timeout;

	WsControlMessage() :
		type((Type)-1),
		messageType((MessageType)-1),
		timeout(-1)
	{
	}

	static WsControlMessage fromVariant(const QVariant &in, bool *ok = 0, QString *errorMessage = 0);
};

#endif
