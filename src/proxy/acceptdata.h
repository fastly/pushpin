/*
 * Copyright (C) 2012-2023 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
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

#ifndef ACCEPTDATA_H
#define ACCEPTDATA_H

#include <QList>
#include "httpheaders.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "inspectdata.h"
#include "zhttprequest.h"

class AcceptData
{
public:
	class Request
	{
	public:
		ZhttpRequest::Rid rid;
		bool https;
		QHostAddress peerAddress;
		QHostAddress logicalPeerAddress;
		bool debug;
		bool isRetry;
		bool autoCrossOrigin;
		QByteArray jsonpCallback;
		bool jsonpExtendedResponse;
		int unreportedTime;

		// zhttp
		int responseCode;
		int inSeq;
		int outSeq;
		int outCredits;
		QVariant userData;

		Request() :
			https(false),
			debug(false),
			isRetry(false),
			autoCrossOrigin(false),
			jsonpExtendedResponse(false),
			unreportedTime(-1),
			responseCode(-1),
			inSeq(-1),
			outSeq(-1),
			outCredits(-1)
		{
		}
	};

	QList<Request> requests;
	HttpRequestData requestData;
	HttpRequestData origRequestData;

	bool haveInspectData;
	InspectData inspectData;

	bool haveResponse;
	HttpResponseData response;

	QByteArray route;
	bool separateStats;
	QByteArray channelPrefix;
	QList<QByteArray> channels;
	bool trusted; // whether a trusted target was used
	bool useSession;
	bool responseSent;
	QVariantList connMaxPackets;

	AcceptData() :
		haveInspectData(false),
		haveResponse(false),
		separateStats(false),
		useSession(false),
		responseSent(false)
	{
	}
};

#endif
