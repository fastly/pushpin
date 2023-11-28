/*
 * Copyright (C) 2016-2020 Fanout, Inc.
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

#ifndef PUBLISHFORMAT_H
#define PUBLISHFORMAT_H

#include <QByteArray>
#include <QString>
#include <QVariant>
#include "httpheaders.h"

class PublishFormat
{
public:
	enum Type
	{
		HttpResponse,
		HttpStream,
		WebSocketMessage
	};

	enum Action
	{
		Send,
		Hint,
        Refresh,
		Close
	};

	enum MessageType
	{
		Text,
		Binary,
		Ping,
		Pong
	};

	Type type;
	Action action; // response/stream/ws
	int code; // response/ws
	QByteArray reason; // response/ws
	HttpHeaders headers; // response
	QByteArray body; // response/stream/ws
	bool haveBodyPatch; // response
	QVariantList bodyPatch; // response
	MessageType messageType; // ws
	bool haveContentFilters;
	QStringList contentFilters; // response/stream/ws

	PublishFormat() :
		type((Type)-1),
		action(Send),
		code(-1),
		haveBodyPatch(false),
		messageType((MessageType)-1),
		haveContentFilters(false)
	{
	}

	PublishFormat(Type _type) :
		type(_type),
		action(Send),
		code(-1),
		haveBodyPatch(false),
		messageType((MessageType)-1),
		haveContentFilters(false)
	{
	}

	static PublishFormat fromVariant(Type type, const QVariant &in, bool *ok = 0, QString *errorMessage = 0);
};

#endif
