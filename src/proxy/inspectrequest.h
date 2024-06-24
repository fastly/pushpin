/*
 * Copyright (C) 2012-2015 Fanout, Inc.
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

	void start(const HttpRequestData &hdata, bool truncated, bool getSession, bool autoShare);

protected:
	virtual void onSuccess();

private:
	class Private;
	Private *d;
};

#endif
