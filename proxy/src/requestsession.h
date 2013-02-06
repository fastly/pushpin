/*
 * Copyright (C) 2012-2013 Fan Out Networks, Inc.
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
#include "m2request.h"

class HttpRequestData;
class InspectData;
class AcceptData;
class InspectManager;
class InspectChecker;
class M2Response;

class RequestSession : public QObject
{
	Q_OBJECT

public:
	RequestSession(InspectManager *inspectManager, InspectChecker *inspectChecker, QObject *parent = 0);
	~RequestSession();

	bool isRetry() const;
	bool isHttps() const;
	QString host() const;
	M2Request::Rid rid() const;
	HttpRequestData requestData() const;
	QByteArray jsonpCallback() const; // non-empty if JSON-P is used

	M2Request *request(); // null if retry mode

	// takes ownership
	void start(M2Request *req);

	// creates an M2Request-less session
	bool setupAsRetry(const M2Request::Rid &rid, const HttpRequestData &hdata, bool https, const QByteArray &jsonpCallback, M2Manager *manager);

	M2Response *createResponse();

	void cannotAccept();

signals:
	void inspected(const InspectData &idata);
	void inspectError();
	void finished();
	void finishedForAccept(const AcceptData &adata);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
