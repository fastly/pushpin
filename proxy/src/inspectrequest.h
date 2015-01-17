/*
 * Copyright (C) 2012-2015 Fanout, Inc.
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

#ifndef INSPECTREQUEST_H
#define INSPECTREQUEST_H

#include <QObject>
#include "zrpcrequest.h"

class HttpRequestData;
class InspectData;
class ZrpcManager;

class InspectRequest : public ZrpcRequest
{
	Q_OBJECT

public:
	InspectRequest(ZrpcManager *manager, QObject *parent = 0);
	~InspectRequest();

	InspectData result() const;

	void start(const HttpRequestData &hdata, bool truncated, bool getSession);

protected:
	virtual void onSuccess();

private:
	class Private;
	Private *d;
};

#endif
