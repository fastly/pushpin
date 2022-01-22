/*
 * Copyright (C) 2012-2022 Fanout, Inc.
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

		// zhttp
		int inSeq;
		int outSeq;
		int outCredits;
		QVariant userData;

		Request() :
			https(false),
			debug(false),
			autoCrossOrigin(false),
			jsonpExtendedResponse(false),
			inSeq(-1),
			outSeq(-1),
			outCredits(-1)
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

	RetryRequestPacket();

	QVariant toVariant() const;
	bool fromVariant(const QVariant &in);
};

#endif
