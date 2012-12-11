/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
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

#ifndef REQUESTSESSION_H
#define REQUESTSESSION_H

#include <QObject>

class M2Request;
class InspectManager;
class InspectData;

class RequestSession : public QObject
{
	Q_OBJECT

public:
	RequestSession(InspectManager *inspectManager, QObject *parent = 0);
	~RequestSession();

	M2Request *request();

	// takes ownership
	void start(M2Request *req);

signals:
	void inspectFinished(const InspectData &idata);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
