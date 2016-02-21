/*
 * Copyright (C) 2015 Fanout, Inc.
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
