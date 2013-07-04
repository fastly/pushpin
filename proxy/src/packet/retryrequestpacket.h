/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifndef RETRYREQUESTPACKET_H
#define RETRYREQUESTPACKET_H

#include <QVariant>
#include <QHostAddress>
#include "httprequestdata.h"
#include "inspectresponsepacket.h"

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
		bool autoCrossOrigin;
		QByteArray jsonpCallback;

		// zhttp
		int inSeq;
		int outSeq;
		int outCredits;
		QVariant userData;

		Request() :
			https(false),
			autoCrossOrigin(false),
			inSeq(-1),
			outSeq(-1),
			outCredits(-1)
		{
		}
	};

	QList<Request> requests;
	HttpRequestData requestData;

	bool haveInspectInfo;
	InspectResponsePacket inspectInfo;

	RetryRequestPacket();

	bool fromVariant(const QVariant &in);
};

#endif
