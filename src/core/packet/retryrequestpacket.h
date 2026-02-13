/*
 * Copyright (C) 2012-2023 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
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

#ifndef RETRYREQUESTPACKET_H
#define RETRYREQUESTPACKET_H

#include <QVariant>
#include <QHostAddress>
#include "httprequestdata.h"

class RetryRequestPacket
{
public:
	typedef QPair<QByteArray, QByteArray> Rid;

	class Request
	{
	public:
		Rid rid;
		bool https;
		QHostAddress peerAddress;
		bool debug;
		bool autoCrossOrigin;
		QByteArray jsonpCallback;
		bool jsonpExtendedResponse;
		int unreportedTime;

		// Zhttp
		int inSeq;
		int outSeq;
		int outCredits;
		bool routerResp;
		QVariant userData;

		Request() :
			https(false),
			debug(false),
			autoCrossOrigin(false),
			jsonpExtendedResponse(false),
			unreportedTime(-1),
			inSeq(-1),
			outSeq(-1),
			outCredits(-1),
			routerResp(false)
		{
		}
	};

	class InspectInfo
	{
	public:
		bool doProxy;
		QByteArray sharingKey;
		QByteArray sid;
		QHash<QByteArray, QByteArray> lastIds;
		QVariant userData;

		InspectInfo() :
			doProxy(false)
		{
		}
	};

	QList<Request> requests;
	HttpRequestData requestData;

	bool haveInspectInfo;
	InspectInfo inspectInfo;

	QByteArray route;
	int retrySeq;

	RetryRequestPacket();

	QVariant toVariant() const;
	bool fromVariant(const QVariant &in);
};

#endif
