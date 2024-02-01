/*
 * Copyright (C) 2012-2023 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#ifndef REQUESTSESSION_H
#define REQUESTSESSION_H

#include <QObject>
#include "zhttprequest.h"
#include "domainmap.h"
#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;
using SignalInt = boost::signals2::signal<void(int)>;
using Connection = boost::signals2::scoped_connection;

class QHostAddress;

namespace Jwt {
	class DecodingKey;
}

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
	void setDefaultUpstreamKey(const Jwt::DecodingKey &key);
	void setXffRules(const XffRule &untrusted, const XffRule &trusted);

	// takes ownership
	void start(ZhttpRequest *req);
	void startRetry(ZhttpRequest *req, bool debug, bool autoCrossOrigin, const QByteArray &jsonpCallback, bool jsonpExtendedResponse, int unreportedTime, int retrySeq);

	void pause();
	void resume();

	void startResponse(int code, const QByteArray &reason, const HttpHeaders &headers);
	void writeResponseBody(const QByteArray &body);
	void endResponseBody();

	void respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body);
	void respondError(int code, const QString &reason, const QString &errorString);
	void respondCannotAccept();

	int unregisterConnection(); // return unreported time

	Signal inspectError;
	boost::signals2::signal<void(const InspectData&)> inspected;
	Signal finishedByAccept;
	SignalInt bytesWritten;
	Signal paused;
	SignalInt headerBytesSent;
	SignalInt bodyBytesSent;
	// this signal means some error was encountered while responding and
	//   that you should not attempt to call further response-related
	//   methods. the object remains in an active state though, and so you
	//   should still wait for finished()
	Signal errorResponding;

signals:
	void finished();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
