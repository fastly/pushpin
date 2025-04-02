/*
 * Copyright (C) 2012-2023 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
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

#include "proxysession.h"

#include <assert.h>
#include <QSet>
#include <QUrl>
#include <QHostAddress>
#include "packet/statspacket.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "qtcompat.h"
#include "bufferlist.h"
#include "log.h"
#include "jwt.h"
#include "inspectdata.h"
#include "acceptdata.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"
#include "zroutes.h"
#include "statusreasons.h"
#include "xffrule.h"
#include "requestsession.h"
#include "proxyutil.h"
#include "statsmanager.h"
#include "acceptrequest.h"
#include "testhttprequest.h"

using std::map;

#define MAX_ACCEPT_REQUEST_BODY 100000

// NOTE: if this value is ever changed, fix enginetest to match
#define MAX_ACCEPT_RESPONSE_BODY 100000

#define MAX_INITIAL_BUFFER 100000
#define MAX_STREAM_BUFFER 100000

class ProxySession::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Stopped,
		Requesting,
		Accepting,
		Responding,
		Responded
	};

	class SessionItem
	{
	public:
		enum State
		{
			WaitingForResponse,
			Responding,
			Responded,
			Errored,
			Pausing,
			Paused
		};

		RequestSession *rs;
		State state;
		bool startedResponse;
		bool unclean;
		int bytesToWrite;
		bool countClientReceivedBytes;
		bool countClientSentBytes;

		SessionItem() :
			rs(0),
			state(WaitingForResponse),
			startedResponse(false),
			unclean(false),
			bytesToWrite(0),
			countClientReceivedBytes(true),
			countClientSentBytes(true)
		{
		}
	};

	struct RequestSessionConnections {
		Connection bytesWrittenConnection;
		Connection errorRespondingConnection;
		Connection pausedConnection;
		Connection finishedConnection;
		Connection headerBytesSentConnection;
		Connection bodyBytesSentConnection;
	};

	struct ZhttpReqConnections {
		Connection readyReadConnection;
		Connection writeBytesChangedConnection;
		Connection errorConnection;
	};

	ProxySession *q;
	State state;
	ZRoutes *zroutes;
	ZhttpManager *zhttpManager;
	RequestSession *inRequest;
	ZrpcManager *acceptManager;
	bool isHttps;
	DomainMap::Entry route;
	QList<DomainMap::Target> targets;
	DomainMap::Target target;
	std::unique_ptr<HttpRequest> zhttpRequest;
	bool addAllowed;
	bool haveInspectData;
	InspectData idata;
	QSet<QByteArray> acceptHeaderPrefixes;
	QSet<QByteArray> acceptContentTypes;
	QSet<SessionItem*> sessionItems;
	bool shared;
	HttpRequestData requestData;
	HttpRequestData origRequestData;
	HttpResponseData responseData;
	HttpResponseData acceptResponseData;
	BufferList requestBody;
	BufferList responseBody;
	QHash<RequestSession*, SessionItem*> sessionItemsBySession;
	QByteArray initialRequestBody;
	bool requestBodySent;
	int total;
	bool buffering;
	QByteArray defaultSigIss;
	Jwt::EncodingKey defaultSigKey;
	bool trustedClient;
	bool intReq;
	bool passthrough;
	bool acceptXForwardedProtocol;
	bool useXForwardedProto;
	bool useXForwardedProtocol;
	XffRule xffRule;
	XffRule xffTrustedRule;
	QList<QByteArray> origHeadersNeedMark;
	bool acceptPushpinRoute;
	QByteArray cdnLoop;
	bool proxyInitialResponse;
	bool acceptAfterResponding;
	std::unique_ptr<AcceptRequest> acceptRequest;
	LogUtil::Config logConfig;
	StatsManager *statsManager;
	Connection inReqReadyReadConnection;
	Connection inReqErrorConnection;
	ZhttpReqConnections zhttpReqConnections;
	Connection finishedConnection;
	map<RequestSession*, RequestSessionConnections> reqSessionConnectionMap;

	Private(ProxySession *_q, ZRoutes *_zroutes, ZrpcManager *_acceptManager, const LogUtil::Config &_logConfig, StatsManager *_statsManager) :
		q(_q),
		state(Stopped),
		zroutes(_zroutes),
		zhttpManager(0),
		inRequest(0),
		acceptManager(_acceptManager),
		isHttps(false),
		addAllowed(true),
		haveInspectData(false),
		shared(false),
		requestBodySent(false),
		total(0),
		trustedClient(false),
		intReq(false),
		passthrough(false),
		acceptXForwardedProtocol(false),
		useXForwardedProto(false),
		useXForwardedProtocol(false),
		acceptPushpinRoute(false),
		proxyInitialResponse(false),
		acceptAfterResponding(false),
		logConfig(_logConfig),
		statsManager(_statsManager)
	{
		acceptHeaderPrefixes += "Grip-";
		acceptContentTypes += "application/grip-instruct";
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		foreach(SessionItem *si, sessionItems)
		{
			// emitting a signal here is gross, but this way the engine cleans up the request sessions
			q->requestSessionDestroyed(si->rs, false);
			delete si->rs;
			delete si;
		}

		sessionItems.clear();
		sessionItemsBySession.clear();

		if(zhttpManager)
		{
			zroutes->removeRef(zhttpManager);
			zhttpManager = 0;
		}
	}

	void add(RequestSession *rs)
	{
		assert(addAllowed);
		assert(!route.isNull());

		SessionItem *si = new SessionItem;
		si->rs = rs;

		// a retried request already had its received bytes counted earlier
		if(rs->isRetry())
			si->countClientReceivedBytes = false;

		// internal requests originate internally and should not have client bytes counted
		if(rs->request()->passthroughData().isValid())
		{
			si->countClientReceivedBytes = false;
			si->countClientSentBytes = false;
		}

		if(!sessionItems.isEmpty())
			shared = true;

		sessionItems += si;
		sessionItemsBySession.insert(rs, si);
		reqSessionConnectionMap[rs] = {
			rs->bytesWritten.connect(boost::bind(&Private::rs_bytesWritten, this, boost::placeholders::_1, rs)),
			rs->errorResponding.connect(boost::bind(&Private::rs_errorResponding, this, rs)),
			rs->paused.connect(boost::bind(&Private::rs_paused, this, rs)),
			rs->finished.connect(boost::bind(&Private::rs_finished, this, rs)),
			rs->headerBytesSent.connect(boost::bind(&Private::rs_headerBytesSent, this, boost::placeholders::_1, rs)),
			rs->bodyBytesSent.connect(boost::bind(&Private::rs_bodyBytesSent, this, boost::placeholders::_1, rs))
		};

		HttpRequestData rsRequestData = rs->requestData();

		if(si->countClientReceivedBytes)
		{
			incCounter(Stats::ClientHeaderBytesReceived, ZhttpManager::estimateRequestHeaderBytes(rsRequestData.method, rsRequestData.uri, rsRequestData.headers));
			incCounter(Stats::ClientContentBytesReceived, rsRequestData.body.size());
		}

		if(state == Stopped)
		{
			isHttps = rs->isHttps();

			requestData = rsRequestData;
			requestBody += requestData.body;
			requestData.body.clear();

			origRequestData = requestData;

			if(!route.asHost.isEmpty())
				ProxyUtil::applyHost(&requestData.uri, route.asHost);

			QByteArray path = requestData.uri.path(QUrl::FullyEncoded).toUtf8();

			if(route.pathRemove > 0)
				path = path.mid(route.pathRemove);

			if(!route.pathPrepend.isEmpty())
				path = route.pathPrepend + path;

			requestData.uri.setPath(QString::fromUtf8(path), QUrl::StrictMode);

			QByteArray sigIss = defaultSigIss;
			Jwt::EncodingKey sigKey = defaultSigKey;

			if(!route.sigIss.isEmpty())
				sigIss = route.sigIss;

			if(!route.sigKey.isNull())
				sigKey = route.sigKey;

			targets = route.targets;

			foreach(const HttpHeader &h, route.headers)
			{
				requestData.headers.removeAll(h.first);
				if(!h.second.isEmpty())
					requestData.headers += HttpHeader(h.first, h.second);
			}

			if(!rs->isRetry())
			{
				inRequest = rs;

				ZhttpRequest *req = inRequest->request();

				inReqReadyReadConnection = req->readyRead.connect(boost::bind(&Private::inRequest_readyRead, this));
				inReqErrorConnection = req->error.connect(boost::bind(&Private::inRequest_error, this));

				requestBody += req->readBody();

				intReq = req->passthroughData().isValid();
			}

			trustedClient = rs->trusted();
			QHostAddress clientAddress = rs->request()->peerAddress();

			ProxyUtil::manipulateRequestHeaders("proxysession", q, &requestData, trustedClient, route, sigIss, sigKey, acceptXForwardedProtocol, useXForwardedProto, useXForwardedProtocol, xffTrustedRule, xffRule, origHeadersNeedMark, acceptPushpinRoute, cdnLoop, clientAddress, idata, route.grip, intReq);

			state = Requesting;
			buffering = true;

			if(trustedClient || !route.grip || intReq)
				passthrough = true;

			initialRequestBody = requestBody.toByteArray();

			if(requestBody.size() > MAX_ACCEPT_REQUEST_BODY)
			{
				requestBody.clear();
				buffering = false;
			}

			tryNextTarget();
		}
		else if(state == Requesting)
		{
			// nothing to do, just wait around until a response comes
		}
		else if(state == Responding)
		{
			// get the session caught up with where we're at

			si->state = SessionItem::Responding;
			si->startedResponse = true;
			rs->startResponse(responseData.code, responseData.reason, responseData.headers);

			if(!responseBody.isEmpty())
			{
				si->bytesToWrite += responseBody.size();
				rs->writeResponseBody(responseBody.toByteArray());
			}
		}
	}

	bool pendingWrites()
	{
		foreach(SessionItem *si, sessionItems)
		{
			if(si->bytesToWrite != -1 && si->bytesToWrite > 0)
				return true;
		}

		return false;
	}

	void tryNextTarget()
	{
		if(targets.isEmpty())
		{
			QString msg = "Error while proxying to origin.";

			QStringList targetStrs;
			foreach(const DomainMap::Target &t, route.targets)
				targetStrs += ProxyUtil::targetToString(t);
			QString dmsg = QString("Unable to connect to any targets. Tried: %1").arg(targetStrs.join(", "));

			rejectAll(502, "Bad Gateway", msg, dmsg);
			return;
		}

		target = targets.takeFirst();

		if(target.overHttp)
		{
			// don't forward WOH requests from client unless trusted

			QByteArray contentType = requestData.headers.get("Content-Type");
			int at = contentType.indexOf(';');
			if(at != -1)
				contentType.truncate(at);

			if(contentType == "application/websocket-events" && !trustedClient)
			{
				rejectAll(403, "Forbidden", "Client not allowed to send WebSocket events directly.");
				return;
			}
		}

		QUrl uri = requestData.uri;
		if(target.ssl)
			uri.setScheme("https");
		else
			uri.setScheme("http");

		if(!target.host.isEmpty())
			ProxyUtil::applyHost(&uri, target.host);

		if(zhttpManager)
		{
			zroutes->removeRef(zhttpManager);
			zhttpManager = 0;
		}

		if(target.type == DomainMap::Target::Test)
		{
			// for test route, auto-adjust path
			if(!route.pathBeg.isEmpty())
			{
				int pathRemove = route.pathBeg.length();
				if(route.pathBeg.endsWith('/'))
					--pathRemove;

				if(pathRemove > 0)
					uri.setPath(uri.path(QUrl::FullyEncoded).mid(pathRemove));
			}

			zhttpRequest = std::make_unique<TestHttpRequest>();
		}
		else
		{
			if(target.type == DomainMap::Target::Custom)
			{
				zhttpManager = zroutes->managerForRoute(target.zhttpRoute);
				log_debug("proxysession: %p forwarding to %s", q, qPrintable(target.zhttpRoute.baseSpec));
			}
			else // Default
			{
				zhttpManager = zroutes->defaultManager();
				log_debug("proxysession: %p forwarding to %s:%d", q, qPrintable(target.connectHost), target.connectPort);
			}

			zroutes->addRef(zhttpManager);

			zhttpRequest = std::unique_ptr<HttpRequest>(zhttpManager->createRequest());
		}

		zhttpReqConnections = {
			zhttpRequest->readyRead.connect(boost::bind(&Private::zhttpRequest_readyRead, this)),
			zhttpRequest->writeBytesChanged.connect(boost::bind(&Private::zhttpRequest_writeBytesChanged, this)),
			zhttpRequest->error.connect(boost::bind(&Private::zhttpRequest_error, this))
		};

		if(target.trusted)
			zhttpRequest->setIgnorePolicies(true);

		if(target.trustConnectHost)
			zhttpRequest->setTrustConnectHost(true);

		if(target.insecure)
			zhttpRequest->setIgnoreTlsErrors(true);

		if(target.type == DomainMap::Target::Default)
		{
			zhttpRequest->setConnectHost(target.connectHost);
			zhttpRequest->setConnectPort(target.connectPort);
		}

		ProxyUtil::applyHostHeader(&requestData.headers, uri);

		incCounter(Stats::ServerHeaderBytesSent, ZhttpManager::estimateRequestHeaderBytes(requestData.method, uri, requestData.headers));

		zhttpRequest->start(requestData.method, uri, requestData.headers);

		requestBodySent = false;

		if(!initialRequestBody.isEmpty())
		{
			incCounter(Stats::ServerContentBytesSent, initialRequestBody.size());

			zhttpRequest->writeBody(initialRequestBody);
		}

		if(!inRequest || (inRequest->request()->isInputFinished() && inRequest->request()->bytesAvailable() == 0))
		{
			// no need to track the primary request anymore
			if(inRequest)
			{
				inReqReadyReadConnection.disconnect();
				inReqErrorConnection.disconnect();
				inRequest = 0;
			}

			requestBodySent = true;
			zhttpRequest->endBody();
		}
	}

	void tryRequestRead()
	{
		// if the state changed before input finished, then
		//   stop reading input
		if(state != Requesting)
			return;

		int maxBytes = buffering ? MAX_STREAM_BUFFER : zhttpRequest->writeBytesAvailable();

		// if we're not buffering, then sync to speed of server
		if(maxBytes == 0)
			return;

		QByteArray buf = inRequest->request()->readBody(maxBytes);
		if(!buf.isEmpty())
		{
			log_debug("proxysession: %p input chunk: %d", q, buf.size());

			SessionItem *si = sessionItemsBySession.value(inRequest);
			assert(si);

			if(si->countClientReceivedBytes)
				incCounter(Stats::ClientContentBytesReceived, buf.size());

			if(buffering)
			{
				if(requestBody.size() + buf.size() > MAX_ACCEPT_REQUEST_BODY)
				{
					requestBody.clear();
					buffering = false;
				}
				else
					requestBody += buf;
			}

			incCounter(Stats::ServerContentBytesSent, buf.size());

			zhttpRequest->writeBody(buf);
		}

		if(!requestBodySent && inRequest->request()->isInputFinished() && inRequest->request()->bytesAvailable() == 0)
		{
			// no need to track the primary request anymore
			inReqReadyReadConnection.disconnect();
			inReqErrorConnection.disconnect();
			inRequest = 0;

			requestBodySent = true;
			zhttpRequest->endBody();
		}
	}

	void cannotAcceptAll()
	{
		assert(state != Responding);
		assert(state != Responded);

		state = Responded;

		foreach(SessionItem *si, sessionItems)
		{
			if(si->state != SessionItem::Errored)
			{
				if(si->state == SessionItem::Paused)
				{
					if(si->startedResponse)
						si->state = SessionItem::Responding;
					else
						si->state = SessionItem::WaitingForResponse;

					si->rs->resume();
				}

				assert(si->state == SessionItem::WaitingForResponse || si->state == SessionItem::Responding);

				if(si->state == SessionItem::WaitingForResponse)
				{
					si->state = SessionItem::Responded;
					si->bytesToWrite = -1;

					si->rs->respondCannotAccept();
				}
				else
				{
					// if we already started responding, then only provide an
					//   error message in debug mode

					if(si->rs->debugEnabled())
					{
						// if debug enabled, append the message at the end.
						//   this may ruin the content, but hey it's debug
						//   mode
						QByteArray buf = "\n\nAccept service unavailable\n";

						si->bytesToWrite += buf.size();
						si->rs->writeResponseBody(buf);
						si->rs->endResponseBody();
					}
					else
					{
						// if debug not enabled, then the best we can do is
						//   disconnect
						si->state = SessionItem::Responded;
						si->unclean = true;
						si->bytesToWrite = -1;
						si->rs->endResponseBody();
					}
				}
			}
		}
	}

	void rejectAll(int code, const QString &reason, const QString &errorMessage, const QString &debugErrorMessage)
	{
		zhttpReqConnections = ZhttpReqConnections();
		// kill the active target request, if any
		zhttpRequest.reset();

		assert(state != Responding);
		assert(state != Responded);

		state = Responded;

		foreach(SessionItem *si, sessionItems)
		{
			if(si->state != SessionItem::Errored)
			{
				if(si->state == SessionItem::Paused)
				{
					if(si->startedResponse)
						si->state = SessionItem::Responding;
					else
						si->state = SessionItem::WaitingForResponse;

					si->rs->resume();
				}

				assert(si->state == SessionItem::WaitingForResponse || si->state == SessionItem::Responding);

				if(si->state == SessionItem::WaitingForResponse)
				{
					si->state = SessionItem::Responded;
					si->bytesToWrite = -1;

					si->rs->respondError(code, reason, si->rs->debugEnabled() ? debugErrorMessage : errorMessage);
				}
				else // Responding
				{
					// if we already started responding, then only provide a
					//   rejection message in debug mode

					if(si->rs->debugEnabled())
					{
						// if debug enabled, append the message at the end.
						//   this may ruin the content, but hey it's debug
						//   mode
						QByteArray buf = "\n\n" + debugErrorMessage.toUtf8() + '\n';

						si->bytesToWrite += buf.size();
						si->rs->writeResponseBody(buf);
						si->rs->endResponseBody();
					}
					else
					{
						// if debug not enabled, then the best we can do is
						//   disconnect
						si->state = SessionItem::Responded;
						si->unclean = true;
						si->bytesToWrite = -1;
						si->rs->endResponseBody();
					}
				}
			}
		}
	}

	void rejectAll(int code, const QString &reason, const QString &errorMessage)
	{
		rejectAll(code, reason, errorMessage, errorMessage);
	}

	void respondAll(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		assert(state != Responding);
		assert(state != Responded);

		state = Responded;

		foreach(SessionItem *si, sessionItems)
		{
			if(si->state != SessionItem::Errored)
			{
				assert(si->state == SessionItem::WaitingForResponse);

				si->state = SessionItem::Responded;
				si->bytesToWrite = -1;
				si->rs->respond(code, reason, headers, body);
			}
		}
	}

	void destroyAll()
	{
		assert(state == Accepting || state == Responding);

		state = Responded;

		foreach(SessionItem *si, sessionItems)
		{
			if(si->state == SessionItem::Paused)
			{
				si->state = SessionItem::Responding;
				si->rs->resume();
			}

			if(si->state == SessionItem::WaitingForResponse || si->state == SessionItem::Responding)
			{
				si->state = SessionItem::Responded;
				si->unclean = true;
				si->bytesToWrite = -1;
				si->rs->endResponseBody();
			}
		}
	}

	// this method emits signals
	void tryResponseRead()
	{
		// if we're not buffering, then don't read (instead, sync to slowest
		//   receiver before reading again)
		if(!buffering && pendingWrites())
			return;

		std::weak_ptr<Private> self = q->d;

		bool wasAllowed = addAllowed;

		if(state == Accepting)
		{
			if(responseBody.size() + zhttpRequest->bytesAvailable() > MAX_ACCEPT_RESPONSE_BODY)
			{
				QByteArray gripHold = responseData.headers.get("Grip-Hold");

				QByteArray gripNextLinkParam;
				foreach(const HttpHeaderParameters &params, responseData.headers.getAllAsParameters("Grip-Link"))
				{
					if(params.count() >= 2 && params.get("rel") == "next")
						gripNextLinkParam = params[0].first;
				}

				bool usingBuildIdFilter = false;
				foreach(const HttpHeaderParameters &params, responseData.headers.getAllAsParameters("Grip-Channel"))
				{
					if(params.count() >= 2)
					{
						bool found = false;
						for(int n = 1; n < params.count(); ++n)
						{
							if(params[n].first == "filter" && params[n].second == "build-id")
							{
								found = true;
								break;
							}
						}
						if(found)
						{
							usingBuildIdFilter = true;
							break;
						}
					}
				}

				if(proxyInitialResponse && (gripHold == "stream" || (gripHold.isEmpty() && !gripNextLinkParam.isEmpty())) && !usingBuildIdFilter)
				{
					// sending the initial response from the proxy means
					//   we need to do some of the handler's job here

					// NOTE: if we ever need to do more than what's
					//   below, we should consider querying the handler
					//   to perform these things while still letting
					//   the proxy send the response body

					// no content length
					responseData.headers.removeAll("Content-Length");

					// interpret grip-status
					QByteArray statusHeader = responseData.headers.get("Grip-Status");
					if(!statusHeader.isEmpty())
					{
						QByteArray codeStr;
						QByteArray reason;

						int at = statusHeader.indexOf(' ');
						if(at != -1)
						{
							codeStr = statusHeader.mid(0, at);
							reason = statusHeader.mid(at + 1);
						}
						else
						{
							codeStr = statusHeader;
						}

						bool _ok;
						responseData.code = codeStr.toInt(&_ok);
						if(!_ok || responseData.code < 0 || responseData.code > 999)
						{
							// this may output a misleading error message
							cannotAcceptAll();
							return;
						}

						if(reason.isEmpty())
							reason = StatusReasons::getReason(responseData.code);

						responseData.reason = reason;
					}

					// strip any grip headers
					for(int n = 0; n < responseData.headers.count(); ++n)
					{
						const HttpHeader &h = responseData.headers[n];

						bool prefixed = false;
						foreach(const QByteArray &hp, acceptHeaderPrefixes)
						{
							if(qstrnicmp(h.first.data(), hp.data(), hp.length()) == 0)
							{
								prefixed = true;
								break;
							}
						}

						if(prefixed)
						{
							responseData.headers.removeAt(n);
							--n; // adjust position
						}
					}

					// we'll let the proxy send normally, then accept afterwards
					acceptAfterResponding = true;
				}
				else
				{
					QString msg = "Error while proxying to origin.";
					QString dmsg = QString("GRIP instruct response too large from %1").arg(ProxyUtil::targetToString(target));

					rejectAll(502, "Bad Gateway", msg, dmsg);
					return;
				}
			}
		}
		else if(state == Responding)
		{
			if(buffering && responseBody.size() + zhttpRequest->bytesAvailable() > MAX_INITIAL_BUFFER)
			{
				responseBody.clear();
				buffering = false;
				addAllowed = false;
			}
		}

		QByteArray buf;
		int maxBytes = (buffering ? MAX_INITIAL_BUFFER - responseBody.size() : MAX_STREAM_BUFFER);
		if(maxBytes > 0)
			buf = zhttpRequest->readBody(maxBytes);

		if(!buf.isEmpty())
		{
			incCounter(Stats::ServerContentBytesReceived, buf.size());

			total += buf.size();
			log_debug("proxysession: %p recv=%d, total=%d, avail=%d", q, buf.size(), total, zhttpRequest->bytesAvailable());

			if(buffering)
				responseBody += buf;
		}

		if(state == Accepting)
		{
			if(acceptAfterResponding)
				startResponse();
		}
		else if(state == Responding)
		{
			log_debug("proxysession: %p writing %d to clients", q, buf.size());

			foreach(SessionItem *si, sessionItems)
			{
				assert(si->state != SessionItem::WaitingForResponse);

				if(si->state == SessionItem::Responding)
				{
					si->bytesToWrite += buf.size();
					si->rs->writeResponseBody(buf);
				}
			}

			if(wasAllowed && !addAllowed)
			{
				q->addNotAllowed();
				if(self.expired())
					return;
			}
		}

		checkIncomingResponseFinished();
	}

	// this method emits signals
	void checkIncomingResponseFinished()
	{
		std::weak_ptr<Private> self = q->d;

		if(zhttpRequest->isFinished() && zhttpRequest->bytesAvailable() == 0)
		{
			log_debug("proxysession: %p response from target finished", q);

			if(!buffering && pendingWrites())
			{
				log_debug("proxysession: %p still stuff left to write, though. we'll wait.", q);
				return;
			}

			zhttpReqConnections = ZhttpReqConnections();			
			zhttpRequest.reset();

			// once the entire response has been received, cut off any new adds
			if(addAllowed)
			{
				addAllowed = false;
				q->addNotAllowed();
				if(self.expired())
					return;
			}

			if(state == Accepting || (state == Responding && acceptAfterResponding))
			{
				state = Accepting;

				if(acceptManager)
				{
					log_debug("we have an acceptmanager");
					foreach(SessionItem *si, sessionItems)
					{
						si->state = SessionItem::Pausing;
						si->rs->pause();
					}
				}
				else
				{
					cannotAcceptAll();
				}
			}
			else if(state == Responding)
			{
				foreach(SessionItem *si, sessionItems)
				{
					assert(si->state != SessionItem::WaitingForResponse);

					if(si->state == SessionItem::Responding)
					{
						si->state = SessionItem::Responded;
						si->rs->endResponseBody();
					}
				}
			}
		}
	}

	void startResponse()
	{
		state = Responding;

		// don't relay these headers. their meaning is handled by
		//   zurl and they only apply to the outgoing hop.
		responseData.headers.removeAll("Connection");
		responseData.headers.removeAll("Keep-Alive");
		responseData.headers.removeAll("Content-Encoding");
		responseData.headers.removeAll("Transfer-Encoding");

		foreach(SessionItem *si, sessionItems)
		{
			si->state = SessionItem::Responding;
			si->startedResponse = true;
			si->rs->startResponse(responseData.code, responseData.reason, responseData.headers);

			if(!responseBody.isEmpty())
			{
				si->bytesToWrite += responseBody.size();
				si->rs->writeResponseBody(responseBody.toByteArray());
			}
		}
	}

	void logFinished(SessionItem *si, bool accepted = false)
	{
		RequestSession *rs = si->rs;

		HttpResponseData resp = rs->responseData();

		LogUtil::RequestData rd;

		// only log route id if explicitly set
		if(route.separateStats)
			rd.routeId = route.id;

		if(accepted)
		{
			rd.status = LogUtil::Accept;
		}
		else if(resp.code != -1 && !si->unclean)
		{
			rd.status = LogUtil::Response;
			rd.responseData = resp;
			rd.responseBodySize = rs->responseBodySize();
		}
		else
		{
			rd.status = LogUtil::Error;
		}

		rd.requestData = rs->requestData();

		rd.targetStr = ProxyUtil::targetToString(target);
		rd.targetOverHttp = target.overHttp;

		rd.retry = rs->isRetry();
		if(shared)
			rd.sharedBy = this;

		rd.fromAddress = rs->peerAddress();

		LogUtil::logRequest(LOG_LEVEL_INFO, rd, logConfig);
	}

	void incCounter(Stats::Counter c, int count = 1)
	{
		if(statsManager)
			statsManager->incCounter(route.statsRoute(), c, count);
	}

public:
	void inRequest_readyRead()
	{
		tryRequestRead();
	}

	void inRequest_error()
	{
		log_warning("proxysession: %p error reading request", q);

		// don't take action here. do that in rs_finished
	}

	void zhttpRequest_readyRead()
	{
		log_debug("proxysession: %p data from target", q);

		if(state == Requesting)
		{
			responseData.code = zhttpRequest->responseCode();
			responseData.reason = zhttpRequest->responseReason();
			responseData.headers = zhttpRequest->responseHeaders();

			QByteArray buf = zhttpRequest->readBody(MAX_INITIAL_BUFFER);

			incCounter(Stats::ServerHeaderBytesReceived, ZhttpManager::estimateResponseHeaderBytes(responseData.code, responseData.reason, responseData.headers));
			incCounter(Stats::ServerContentBytesReceived, buf.size());

			responseBody += buf;
			total += buf.size();

			acceptResponseData = responseData;

			log_debug("proxysession: %p recv total: %d", q, total);

			bool doAccept = false;
			if(!passthrough)
			{
				QByteArray contentType = responseData.headers.get("Content-Type");
				int at = contentType.indexOf(';');
				if(at != -1)
					contentType = contentType.mid(0, at);

				if(acceptContentTypes.contains(contentType))
				{
					doAccept = true;
				}
				else
				{
					foreach(const HttpHeader &h, responseData.headers)
					{
						foreach(const QByteArray &hp, acceptHeaderPrefixes)
						{
							if(qstrnicmp(h.first.data(), hp.data(), hp.length()) == 0)
							{
								doAccept = true;
								break;
							}
						}

						if(doAccept)
							break;
					}
				}
			}

			if(doAccept)
			{
				if(!buffering)
				{
					rejectAll(400, "Bad Request", "Request too large to accept GRIP instruct.");
					return;
				}

				state = Accepting;
			}
			else
			{
				startResponse();
			}
		}

		assert(state == Accepting || state == Responding);

		tryResponseRead();
	}

	void zhttpRequest_writeBytesChanged()
	{
		if(inRequest)
			tryRequestRead();
	}

	void zhttpRequest_error()
	{
		ZhttpRequest::ErrorCondition e = zhttpRequest->errorCondition();
		log_debug("proxysession: %p target error state=%d, condition=%d", q, (int)state, (int)e);

		if(state == Requesting || state == Accepting)
		{
			bool tryAgain = false;

			switch(e)
			{
				case ZhttpRequest::ErrorLengthRequired:
					rejectAll(411, "Length Required", "Must provide Content-Length header.");
					break;
				case ZhttpRequest::ErrorPolicy:
					rejectAll(502, "Bad Gateway", "Error while proxying to origin.", "Error: Origin host/IP blocked.");
					break;
				case ZhttpRequest::ErrorConnect:
				case ZhttpRequest::ErrorConnectTimeout:
				case ZhttpRequest::ErrorTls:
					// it should not be possible to get one of these errors while accepting
					assert(state == Requesting);
					tryAgain = true;
					break;
				case ZhttpRequest::ErrorTimeout:
					rejectAll(502, "Bad Gateway", "Error while proxying to origin.", "Error: zhttp service for route is unreachable.");
					break;
				default:
					rejectAll(502, "Bad Gateway", "Error while proxying to origin.");
					break;
			}

			if(tryAgain)
				tryNextTarget();
		}
		else if(state == Responding)
		{
			// if we're already responding, then we can't reply with an error
			destroyAll();
		}
	}

	void rs_bytesWritten(int count, RequestSession *rs)
	{
		log_debug("proxysession: %p response bytes written id=%s: %d", q, rs->rid().second.data(), count);

		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		if(si->bytesToWrite != -1)
		{
			si->bytesToWrite -= count;
			assert(si->bytesToWrite >= 0);
		}

		if(zhttpRequest)
			tryResponseRead();
	}

	void rs_finished(RequestSession *rs)
	{
		log_debug("proxysession: %p response finished id=%s", q, rs->rid().second.data());

		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		if(!intReq)
			logFinished(si);

		std::weak_ptr<Private> self = q->d;
		q->requestSessionDestroyed(si->rs, false);
		if(self.expired())
			return;

		ZhttpRequest *req = rs->request();
		bool wasInputRequest = (req && inRequest && req == inRequest->request());

		sessionItemsBySession.remove(rs);
		sessionItems.remove(si);
		reqSessionConnectionMap.erase(rs);
		delete rs;

		delete si;

		if(sessionItems.isEmpty())
		{
			log_debug("proxysession: %p finished by passthrough", q);
			q->finished();
		}
		else if(wasInputRequest)
		{
			// this should never happen. for there to be more than
			//   one SessionItem, inRequest must be 0.
			assert(0);

			rejectAll(500, "Internal Server Error", "Input request failed.");
		}
	}

	void rs_paused(RequestSession *rs)
	{
		log_debug("proxysession: %p response paused id=%s", q, rs->rid().second.data());

		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		assert(si->state == SessionItem::Pausing);
		si->state = SessionItem::Paused;

		bool allPaused = true;
		foreach(SessionItem *si, sessionItems)
		{
			if(si->state != SessionItem::Paused)
			{
				allPaused = false;
				break;
			}
		}

		if(allPaused)
		{
			assert(!acceptRequest);

			QByteArray sigIss = defaultSigIss;
			Jwt::EncodingKey sigKey = defaultSigKey;

			if(!route.sigIss.isEmpty())
				sigIss = route.sigIss;

			if(!route.sigKey.isNull())
				sigKey = route.sigKey;

			acceptResponseData.body = responseBody.take();

			AcceptData adata;

			foreach(SessionItem *si, sessionItems)
			{
				int unreportedTime = -1;

				if(!statsManager->connectionSendEnabled())
					unreportedTime = si->rs->unregisterConnection();

				ZhttpRequest::ServerState ss = si->rs->request()->serverState();

				AcceptData::Request areq;
				areq.rid = si->rs->rid();
				areq.https = si->rs->isHttps();
				areq.peerAddress = si->rs->peerAddress();
				areq.logicalPeerAddress = si->rs->logicalPeerAddress();
				areq.debug = si->rs->debugEnabled();
				areq.isRetry = si->rs->isRetry();
				areq.autoCrossOrigin = si->rs->autoCrossOrigin();
				areq.jsonpCallback = si->rs->jsonpCallback();
				areq.jsonpExtendedResponse = si->rs->jsonpExtendedResponse();
				areq.unreportedTime = unreportedTime;
				areq.responseCode = ss.responseCode;
				areq.inSeq = ss.inSeq;
				areq.outSeq = ss.outSeq;
				areq.outCredits = ss.outCredits;
				areq.userData = ss.userData;
				adata.requests += areq;
			}

			adata.requestData = requestData;
			adata.requestData.body = requestBody.take();
			adata.origRequestData = origRequestData;
			adata.origRequestData.body = adata.requestData.body;

			adata.haveResponse = true;
			adata.response = acceptResponseData;

			if(haveInspectData)
			{
				adata.haveInspectData = true;
				adata.inspectData = idata;
			}

			adata.route = route.id;
			adata.separateStats = route.separateStats;
			adata.channelPrefix = route.prefix;
			adata.logLevel = route.logLevel;
			foreach(const QString &s, target.subscriptions)
				adata.channels += s.toUtf8();
			adata.trusted = target.trusted;
			adata.useSession = route.session;
			adata.responseSent = acceptAfterResponding;

			if(!statsManager->connectionSendEnabled())
			{
				// flush max. the count will include the connections we just unregistered
				adata.connMaxPackets += statsManager->getConnMaxPacket(route.statsRoute()).toVariant();

				// flush max again to get the count without the connections
				adata.connMaxPackets += statsManager->getConnMaxPacket(route.statsRoute()).toVariant();
			}

			acceptRequest = std::make_unique<AcceptRequest>(acceptManager);
			finishedConnection = acceptRequest->finished.connect(boost::bind(&Private::acceptRequest_finished, this));
			acceptRequest->start(adata);
		}
	}

	void rs_errorResponding(RequestSession *rs)
	{
		log_debug("proxysession: %p response error id=%s", q, rs->rid().second.data());

		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		assert(si->state != SessionItem::Errored);

		// flag that we should stop attempting to respond
		si->state = SessionItem::Errored;
		si->bytesToWrite = -1;

		// don't destroy the RequestSession here. a finished signal will arrive next.
	}

	void rs_headerBytesSent(int count, RequestSession *rs)
	{
		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		if(si->countClientSentBytes)
			incCounter(Stats::ClientHeaderBytesSent, count);
	}

	void rs_bodyBytesSent(int count, RequestSession *rs)
	{
		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		if(si->countClientSentBytes)
			incCounter(Stats::ClientContentBytesSent, count);
	}

	void acceptRequest_finished()
	{
		if(acceptRequest->success())
		{
			AcceptRequest::ResponseData rdata = acceptRequest->result();

			finishedConnection.disconnect();
			acceptRequest.reset();

			if(rdata.accepted)
			{
				foreach(SessionItem *si, sessionItems)
					logFinished(si, true);

				// the requests were paused, so deleting them will leave the peer sessions active

				QList<RequestSession*> toDestroy;
				foreach(SessionItem *si, sessionItems)
				{
					toDestroy += si->rs;
					delete si;
				}

				sessionItems.clear();
				sessionItemsBySession.clear();

				std::weak_ptr<Private> self = q->d;
				foreach(RequestSession *rs, toDestroy)
				{
					q->requestSessionDestroyed(rs, true);
					reqSessionConnectionMap.erase(rs);
					delete rs;
					if(self.expired())
						return;
				}

				log_debug("proxysession: %p finished for accept", q);
				cleanup();
				q->finished();
			}
			else
			{
				if(acceptAfterResponding)
				{
					// wake up receivers and append
					foreach(SessionItem *si, sessionItems)
					{
						si->state = SessionItem::Responded;
						si->rs->resume();

						if(rdata.response.code != -1)
							si->rs->writeResponseBody(rdata.response.body);

						si->bytesToWrite = -1;
						si->rs->endResponseBody();
					}
				}
				else
				{
					if(rdata.response.code != -1)
					{
						// wake up receivers
						foreach(SessionItem *si, sessionItems)
						{
							si->state = SessionItem::WaitingForResponse;
							si->rs->resume();
						}

						respondAll(rdata.response.code, rdata.response.reason, rdata.response.headers, rdata.response.body);
					}
					else
					{
						cannotAcceptAll();
					}
				}
			}
		}
		else
		{
			// wake up receivers and reject

			if(acceptRequest->errorCondition() == ZrpcRequest::ErrorFormat && typeId(((ZrpcRequest *)acceptRequest.get())->result()) == QMetaType::QByteArray)
			{
				QString errorString = QString::fromUtf8(((ZrpcRequest *)acceptRequest.get())->result().toByteArray());
				QString msg = "Error while proxying to origin.";
				QString dmsg = QString("Failed to parse accept instructions: %1").arg(errorString);

				rejectAll(502, "Bad Gateway", msg, dmsg);
			}
			else
			{
				cannotAcceptAll();
			}

			finishedConnection.disconnect();
			acceptRequest.reset();
		}
	}
};

ProxySession::ProxySession(ZRoutes *zroutes, ZrpcManager *acceptManager, const LogUtil::Config &logConfig, StatsManager *statsManager)
{
	d = std::make_shared<Private>(this, zroutes, acceptManager, logConfig, statsManager);
}

ProxySession::~ProxySession() = default;

void ProxySession::setRoute(const DomainMap::Entry &route)
{
	d->route = route;
}

void ProxySession::setDefaultSigKey(const QByteArray &iss, const Jwt::EncodingKey &key)
{
	d->defaultSigIss = iss;
	d->defaultSigKey = key;
}

void ProxySession::setAcceptXForwardedProtocol(bool enabled)
{
	d->acceptXForwardedProtocol = enabled;
}

void ProxySession::setUseXForwardedProtocol(bool protoEnabled, bool protocolEnabled)
{
	d->useXForwardedProto = protoEnabled;
	d->useXForwardedProtocol = protocolEnabled;
}

void ProxySession::setXffRules(const XffRule &untrusted, const XffRule &trusted)
{
	d->xffRule = untrusted;
	d->xffTrustedRule = trusted;
}

void ProxySession::setOrigHeadersNeedMark(const QList<QByteArray> &names)
{
	d->origHeadersNeedMark = names;
}

void ProxySession::setAcceptPushpinRoute(bool enabled)
{
	d->acceptPushpinRoute = enabled;
}

void ProxySession::setCdnLoop(const QByteArray &value)
{
	d->cdnLoop = value;
}

void ProxySession::setProxyInitialResponseEnabled(bool enabled)
{
	d->proxyInitialResponse = enabled;
}

void ProxySession::setInspectData(const InspectData &idata)
{
	d->haveInspectData = true;
	d->idata = idata;
}

void ProxySession::add(RequestSession *rs)
{
	d->add(rs);
}

#include "proxysession.moc"
