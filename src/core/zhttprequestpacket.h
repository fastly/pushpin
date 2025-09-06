/*
 * Copyright (C) 2012-2016 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#ifndef ZHTTPREQUESTPACKET_H
#define ZHTTPREQUESTPACKET_H

#include <QUrl>
#include <QVariant>
#include <QHostAddress>
#include "httpheaders.h"

class ZhttpRequestPacket
{
public:
	class Id
	{
	public:
		QByteArray id;
		int seq;

		Id() :
			seq(-1)
		{
		}

		Id(const QByteArray &_id, int _seq = -1) :
			id(_id),
			seq(_seq)
		{
		}
	};

	enum Type
	{
		Data,
		Error,
		Credit,
		KeepAlive,
		Cancel,
		HandoffStart,
		HandoffProceed,
		Close, // WebSocket
		Ping, // WebSocket
		Pong // WebSocket
	};

	QByteArray from;
	QList<Id> ids;

	Type type;
	QByteArray condition;

	int credits;
	bool more;
	bool stream;
	bool routerResp;
	int maxSize;
	int timeout;

	QString method;
	QUrl uri;
	HttpHeaders headers;
	QByteArray body;

	QByteArray contentType; // WebSocket
	int code; // WebSocket

	QVariant userData;

	QHostAddress peerAddress;
	int peerPort;

	QString connectHost;
	int connectPort;
	bool ignorePolicies;
	bool trustConnectHost;
	bool ignoreTlsErrors;
	QString clientCert;
	QString clientKey;
	bool followRedirects;
	QVariant passthrough; // if valid, may contain pushpin-specific passthrough info
	bool multi;
	bool quiet;

	ZhttpRequestPacket() :
		type((Type)-1),
		credits(-1),
		more(false),
		stream(false),
		routerResp(false),
		maxSize(-1),
		timeout(-1),
		code(-1),
		peerPort(-1),
		connectPort(-1),
		ignorePolicies(false),
		trustConnectHost(false),
		ignoreTlsErrors(false),
		followRedirects(false),
		multi(false),
		quiet(false)
	{
	}

	QVariant toVariant() const;
	bool fromVariant(const QVariant &in);
};

#endif
