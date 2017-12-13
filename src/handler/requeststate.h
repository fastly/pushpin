/*
 * Copyright (C) 2016 Fanout, Inc.
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
		jsonpExtendedResponse(false)
	{
	}

	static RequestState fromVariant(const QVariant &in);
};

#endif
