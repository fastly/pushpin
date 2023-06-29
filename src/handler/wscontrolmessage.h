/*
 * Copyright (C) 2016-2019 Fanout, Inc.
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
	QByteArray keepAliveMode;

	WsControlMessage() :
		type((Type)-1),
		messageType((MessageType)-1),
		timeout(-1)
	{
	}

	static WsControlMessage fromVariant(const QVariant &in, bool *ok = 0, QString *errorMessage = 0);
};

#endif
