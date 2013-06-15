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

#ifndef ACCEPTDATA_H
#define ACCEPTDATA_H

#include <QList>
#include "httpheaders.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "inspectdata.h"
#include "m2request.h"

class AcceptData
{
public:
	class Request
	{
	public:
		M2Request::Rid rid;
		bool https;
		QHostAddress peerAddress;
		QByteArray jsonpCallback;

		Request() :
			https(false)
		{
		}
	};

	QList<Request> requests;
	HttpRequestData requestData;

	bool haveInspectData;
	InspectData inspectData;

	bool haveResponse;
	HttpResponseData response;

	AcceptData() :
		haveInspectData(false),
		haveResponse(false)
	{
	}
};

#endif
