/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
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
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#ifndef REQUESTSESSION_H
#define REQUESTSESSION_H

#include <QObject>
#include "zhttprequest.h"
#include "domainmap.h"

class QHostAddress;

class HttpRequestData;
class HttpResponseData;
class SockJsManager;
class InspectData;
class AcceptData;
class ZrpcManager;
class ZrpcChecker;
class StatsManager;
class XffRule;

class RequestSession : public QObject
{
	Q_OBJECT

public:
	RequestSession(StatsManager *stats, QObject *parent = 0);
	RequestSession(DomainMap *domainMap, SockJsManager *sockJsManager, ZrpcManager *inspectManager, ZrpcChecker *inspectChecker, ZrpcManager *accept, StatsManager *stats, QObject *parent = 0);
	~RequestSession();

	bool isRetry() const;
	bool isHttps() const;
	bool isSockJs() const;
	bool trusted() const;
	QHostAddress peerAddress() const;
	QHostAddress logicalPeerAddress() const;
	ZhttpRequest::Rid rid() const;
	HttpRequestData requestData() const;
	HttpResponseData responseData() const;
	int responseBodySize() const;
	bool debugEnabled() const;
	bool autoCrossOrigin() const;
	QByteArray jsonpCallback() const; // non-empty if JSON-P is used
	bool jsonpExtendedResponse() const;
	bool haveCompleteRequestBody() const;
	DomainMap::Entry route() const;

	ZhttpRequest *request();

	void setDebugEnabled(bool enabled);
	void setAutoCrossOrigin(bool enabled);
	void setPrefetchSize(int size);
	void setRoute(const DomainMap::Entry &route);
	void setRouteId(const QString &routeId);
	void setAutoShare(bool enabled);
	void setAccepted(bool enabled);
	void setDefaultUpstreamKey(const QByteArray &key);
	void setXffRules(const XffRule &untrusted, const XffRule &trusted);

	// takes ownership
	void start(ZhttpRequest *req);
	void startRetry(ZhttpRequest *req, bool debug, bool autoCrossOrigin, const QByteArray &jsonpCallback, bool jsonpExtendedResponse);

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
