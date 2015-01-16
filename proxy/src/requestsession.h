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
#include "zhttprequest.h"
#include "domainmap.h"

class QHostAddress;

class HttpRequestData;
class InspectData;
class AcceptData;
class ZrpcManager;
class ZrpcChecker;

class RequestSession : public QObject
{
	Q_OBJECT

public:
	RequestSession(DomainMap *domainMap, ZrpcManager *inspectManager, ZrpcChecker *inspectChecker, ZrpcManager *accept, QObject *parent = 0);
	~RequestSession();

	bool isRetry() const;
	bool isHttps() const;
	QHostAddress peerAddress() const;
	ZhttpRequest::Rid rid() const;
	HttpRequestData requestData() const;
	bool autoCrossOrigin() const;
	QByteArray jsonpCallback() const; // non-empty if JSON-P is used
	bool jsonpExtendedResponse() const;
	bool haveCompleteRequestBody() const;
	DomainMap::Entry route() const;

	ZhttpRequest *request();

	void setAutoCrossOrigin(bool enabled);

	// takes ownership
	void start(ZhttpRequest *req);
	void startRetry(ZhttpRequest *req, bool autoCrossOrigin, const QByteArray &jsonpCallback, bool jsonpExtendedResponse);

	void pause();
	void resume();

	void startResponse(int code, const QByteArray &reason, const HttpHeaders &headers);
	void writeResponseBody(const QByteArray &body);
	void endResponseBody();

	void respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body);
	void respondError(int code, const QString &reason, const QString &errorString);
	void respondCannotAccept();

signals:
	void inspected(const InspectData &idata);
	void inspectError();
	void finished();
	void finishedByAccept();
	void bytesWritten(int count);
	void paused();

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
