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

#ifndef INSPECTCHECKER_H
#define INSPECTCHECKER_H

#include <QObject>

class InspectRequest;

// all inspect requests should be passed to this class for monitoring. use
//   watch() to have it monitor a request, but not own it. use give() to have
//   this class take ownership of an already-watched request.

class InspectChecker : public QObject
{
	Q_OBJECT

public:
	InspectChecker(QObject *parent = 0);
	~InspectChecker();

	bool isInterfaceAvailable() const;
	void setInterfaceAvailable(bool available);

	void watch(InspectRequest *req);
	void give(InspectRequest *req);

private:
	class Private;
	Private *d;
};

#endif
