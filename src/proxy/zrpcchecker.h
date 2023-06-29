/*
 * Copyright (C) 2015 Fanout, Inc.
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

#ifndef ZRPCCHECKER_H
#define ZRPCCHECKER_H

#include <QObject>

class ZrpcRequest;

// all requests should be passed to this class for monitoring. use
//   watch() to have it monitor a request, but not own it. use give() to have
//   this class take ownership of an already-watched request.

class ZrpcChecker : public QObject
{
	Q_OBJECT

public:
	ZrpcChecker(QObject *parent = 0);
	~ZrpcChecker();

	bool isInterfaceAvailable() const;
	void setInterfaceAvailable(bool available);

	void watch(ZrpcRequest *req);
	void give(ZrpcRequest *req);

private:
	class Private;
	Private *d;
};

#endif
