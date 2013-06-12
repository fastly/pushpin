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

#ifndef REQUESTSESSION_H
#define REQUESTSESSION_H

#include <QObject>
#include "m2request.h"

class QHostAddress;

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
	QHostAddress peerAddress() const;
	QString host() const;
	M2Request::Rid rid() const;
	HttpRequestData requestData() const;
	QByteArray jsonpCallback() const; // non-empty if JSON-P is used

	M2Request *request(); // null if retry mode

	void setAutoCrossOrigin(bool enabled);

	// takes ownership
	void start(M2Request *req);

	// creates an M2Request-less session
	bool setupAsRetry(const M2Request::Rid &rid, const HttpRequestData &hdata, bool https, const QHostAddress &peerAddress, const QByteArray &jsonpCallback, M2Manager *manager);

	void startResponse(int code, const QByteArray &status, const HttpHeaders &headers);
	void writeResponseBody(const QByteArray &body);
	void endResponseBody();

	void respondError(int code, const QString &status, const QString &errorString);
	void respondCannotAccept();

signals:
	void inspected(const InspectData &idata);
	void inspectError();
	void finished();
	void finishedForAccept(const AcceptData &adata);
	void bytesWritten(int count);

	// this signal means some error was encountered while responding and
	//   that you should not attempt to call further response-related
	//   methods. the object remains in an active state though, and so you
	//   should still wait for finished()
	void errorResponding();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
