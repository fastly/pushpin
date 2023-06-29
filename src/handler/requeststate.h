/*
 * Copyright (C) 2016-2023 Fanout, Inc.
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

#ifndef REQUESTSTATE_H
#define REQUESTSTATE_H

#include <QByteArray>
#include <QVariant>
#include <QHostAddress>
#include "zhttprequest.h"

class RequestState
{
public:
	ZhttpRequest::Rid rid;
	int responseCode;
	int inSeq;
	int outSeq;
	int outCredits;
	QHostAddress peerAddress;
	QHostAddress logicalPeerAddress;
	bool isHttps;
	bool debug;
	bool isRetry;
	bool autoCrossOrigin;
	QByteArray jsonpCallback;
	bool jsonpExtendedResponse;
	int unreportedTime;
	QVariant userData;

	RequestState() :
		responseCode(-1),
		inSeq(0),
		outSeq(0),
		outCredits(0),
		isHttps(false),
		debug(false),
		isRetry(false),
		autoCrossOrigin(false),
		jsonpExtendedResponse(false),
		unreportedTime(-1)
	{
	}

	static RequestState fromVariant(const QVariant &in);
};

#endif
