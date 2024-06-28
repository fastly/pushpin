/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifndef M2REQUESTPACKET_H
#define M2REQUESTPACKET_H

#include <QVariant>
#include <QHostAddress>
#include "httpheaders.h"

class M2RequestPacket
{
public:
	enum Type
	{
		HttpRequest,
		WebSocketHandshake, // body will contain accept token
		WebSocketFrame,
		Disconnect,
		Credits
	};

	QByteArray sender;
	QByteArray id;

	Type type;

	QHostAddress remoteAddress;
	QByteArray scheme;
	QByteArray version;
	QString method;
	QByteArray uri;
	HttpHeaders headers;
	QByteArray body;

	QString uploadFile;
	bool uploadDone;

	int uploadStreamOffset;
	bool uploadStreamDone;

	int downloadCredits;

	int frameFlags;

	M2RequestPacket();

	bool fromByteArray(const QByteArray &in);
};

#endif
