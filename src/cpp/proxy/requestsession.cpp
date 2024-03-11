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

#include "requestsession.h"

#include <assert.h>
#include <QPointer>
#include <QUrl>
#include <QHostAddress>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "qtcompat.h"
#include "bufferlist.h"
#include "log.h"
#include "layertracker.h"
#include "sockjsmanager.h"
#include "inspectdata.h"
#include "acceptdata.h"
#include "zhttpmanager.h"
#include "zrpcmanager.h"
#include "zrpcchecker.h"
#include "inspectrequest.h"
#include "acceptrequest.h"
#include "statsmanager.h"
#include "cors.h"
#include "proxyutil.h"
#include "xffrule.h"

#define MAX_SHARED_REQUEST_BODY 100000
#define MAX_ACCEPT_REQUEST_BODY 100000

static int fromHex(char c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	else if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	else if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	else
		return -1;
}

static QByteArray parsePercentEncoding(const QByteArray &in)
{
	QByteArray out;

	for(int n = 0; n < in.size(); ++n)
	{
		char c = in[n];

		if(c == '%')
		{
			if(n + 2 >= in.size())
				return QByteArray();

			int hi = fromHex(in[n + 1]);
			if(hi == -1)
				return QByteArray();

			int lo = fromHex(in[n + 2]);
			if(lo == -1)
				return QByteArray();

			unsigned char val = (hi << 4) + lo;
			out += val;

			n += 2; // adjust position
		}
		else
			out += c;
	}

	return out;
}

static bool validMethod(const QString &in)
{
	if(in.isEmpty())
		return false;

	for(int n = 0; n < in.size(); ++n)
	{
		if(!in[n].isPrint())
			return false;
	}

	return true;
}

static QByteArray serializeJsonString(const QString &s)
{
	QByteArray tmp = QJsonDocument(QJsonArray::fromVariantList(QVariantList() << s)).toJson(QJsonDocument::Compact);

	assert(tmp.length() >= 4);
	assert(tmp[0] == '[' && tmp[tmp.length() - 1] == ']');
	assert(tmp[1] == '"' && tmp[tmp.length() - 2] == '"');

	return tmp.mid(1, tmp.length() - 2);
}

static QByteArray ridToString(const QPair<QByteArray, QByteArray> &rid)
{
	return rid.first + ':' + rid.second;
}

class RequestSession::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Stopped,
		Prefetching,
		Inspecting,
		Receiving,
		ReceivingForAccept,
		Accepting,
		WaitingForResponse,
		RespondingStart,
		Responding,
		RespondingInternal
	};

	struct ZhttpReqConnections {
		Connection readyReadConnection;
		Connection pausedConnection;
		Connection errorConnection;
		Connection bytesWrittenConnection;
	};

	RequestSession *q;
	int workerId;
	State state;
	ZhttpRequest::Rid rid;
	DomainMap *domainMap;
	SockJsManager *sockJsManager;
	ZrpcManager *inspectManager;
	ZrpcChecker *inspectChecker;
	ZrpcManager *acceptManager;
	StatsManager *stats;
	ZhttpRequest *zhttpRequest;
	HttpRequestData requestData;
	Jwt::DecodingKey defaultUpstreamKey;
	bool trusted;
	QHostAddress peerAddress;
	QHostAddress logicalPeerAddress;
	DomainMap::Entry route;
	QString routeId;
	bool debug;
	bool autoCrossOrigin;
	InspectRequest *inspectRequest;
	InspectData idata;
	AcceptRequest *acceptRequest;
	BufferList in;
	QByteArray jsonpCallback;
	bool jsonpExtendedResponse;
	HttpResponseData responseData;
	int responseBodySize;
	BufferList out;
	bool responseBodyFinished;
	bool pendingResponseUpdate;
	LayerTracker jsonpTracker;
	bool isRetry;
	QList<QByteArray> jsonpExtractableHeaders;
	int prefetchSize;
	bool needPause;
	bool connectionRegistered;
	bool accepted;
	bool passthrough;
	bool autoShare;
	XffRule xffRule;
	XffRule xffTrustedRule;
	bool isSockJs;
	ZhttpReqConnections zhttpReqConnections;
	Connection inspectFinishedConnection;
	Connection acceptFinishedConnection;

	Private(RequestSession *_q, int _workerId, DomainMap *_domainMap = 0, SockJsManager *_sockJsManager = 0, ZrpcManager *_inspectManager = 0, ZrpcChecker *_inspectChecker = 0, ZrpcManager *_acceptManager = 0, StatsManager *_stats = 0) :
		QObject(_q),
		q(_q),
		workerId(_workerId),
		state(Stopped),
		domainMap(_domainMap),
		sockJsManager(_sockJsManager),
		inspectManager(_inspectManager),
		inspectChecker(_inspectChecker),
		acceptManager(_acceptManager),
		stats(_stats),
		zhttpRequest(0),
		trusted(false),
		debug(false),
		autoCrossOrigin(false),
		inspectRequest(0),
		acceptRequest(0),
		jsonpExtendedResponse(false),
		responseBodySize(0),
		responseBodyFinished(false),
		pendingResponseUpdate(false),
		isRetry(false),
		prefetchSize(0),
		needPause(false),
		connectionRegistered(false),
		accepted(false),
		passthrough(false),
		autoShare(false),
		isSockJs(false)
	{
		jsonpExtractableHeaders += "Cache-Control";
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(zhttpRequest)
		{
			zhttpReqConnections = ZhttpReqConnections();
			delete zhttpRequest;
			zhttpRequest = 0;
		}

		if(inspectRequest)
		{
			inspectFinishedConnection.disconnect();
			inspectChecker->give(inspectRequest);
			inspectRequest = 0;
		}

		if(stats && connectionRegistered)
		{
			connectionRegistered = false;

			QByteArray cid = ridToString(rid);

			// refresh before remove, to ensure transition
			if(accepted)
				stats->refreshConnection(cid);

			// linger if accepted
			bool linger = accepted && stats->connectionSendEnabled();
			stats->removeConnection(cid, linger);
		}

		state = Stopped;
	}

	void start(ZhttpRequest *req)
	{
		zhttpRequest = req;
		rid = req->rid();
		passthrough = req->passthroughData().isValid();

		requestData.method = req->requestMethod();
		requestData.uri = req->requestUri();
		requestData.headers = req->requestHeaders();

		trusted = ProxyUtil::checkTrustedClient("requestsession", q, requestData, defaultUpstreamKey);

		peerAddress = req->peerAddress();
		logicalPeerAddress = ProxyUtil::getLogicalAddress(requestData.headers, trusted ? xffTrustedRule : xffRule, peerAddress);

		log_debug("worker %d: IN id=%s, %s %s", workerId, rid.second.data(), qPrintable(requestData.method), requestData.uri.toEncoded().data());

		bool isHttps = (requestData.uri.scheme() == "https");
		QString host = requestData.uri.host();

		if(route.isNull() && domainMap)
		{
			QByteArray encPath = requestData.uri.path(QUrl::FullyEncoded).toUtf8();

			// look up the route
			if(!routeId.isEmpty() && !domainMap->isIdShared(routeId))
				route = domainMap->entry(routeId);
			else
				route = domainMap->entry(DomainMap::Http, isHttps, host, encPath);

			// before we do anything else, see if this is a sockjs request
			if(!route.isNull() && !route.sockJsPath.isEmpty() && encPath.startsWith(route.sockJsPath))
			{
				isSockJs = true;
				sockJsManager->giveRequest(zhttpRequest, route.sockJsPath.length(), route.sockJsAsPath, route);
				zhttpRequest = 0;
				QMetaObject::invokeMethod(this, "doFinished", Qt::QueuedConnection);
				return;
			}
		}

		zhttpReqConnections.pausedConnection = zhttpRequest->paused.connect(boost::bind(&Private::zhttpRequest_paused, this));
		zhttpReqConnections.errorConnection = zhttpRequest->error.connect(boost::bind(&Private::zhttpRequest_error, this));

		if(!route.isNull())
		{
			if(route.debug)
				debug = true;

			if(route.autoCrossOrigin)
				autoCrossOrigin = true;
		}

		if(autoCrossOrigin)
		{
			DomainMap::JsonpConfig config;
			if(!route.isNull())
				config = route.jsonpConfig;

			bool ok = false;
			QString str;
			tryApplyJsonp(config, &ok, &str);
			if(!ok)
			{
				state = WaitingForResponse;
				respondBadRequest(str);
				return;
			}
		}

		// NOTE: per the license, this functionality may not be removed as it
		//   is the interface for the copyright notice
		if(requestData.headers.contains("Pushpin-Check"))
		{
			QString str =
			"Copyright (C) 2012-2023 Fanout, Inc.\n"
			"Copyright (C) 2023 Fastly, Inc.\n"
			"\n"
			"Pushpin is licensed under the Apache License, Version 2.0 (the \"License\");\n"
			"you may not use this software except in compliance with the License.\n"
			"You may obtain a copy of the License at\n"
			"\n"
			"    http://www.apache.org/licenses/LICENSE-2.0\n"
			"\n"
			"Unless required by applicable law or agreed to in writing, software\n"
			"distributed under the License is distributed on an \"AS IS\" BASIS,\n"
			"WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n"
			"See the License for the specific language governing permissions and\n"
			"limitations under the License.\n";

			state = WaitingForResponse;
			respondSuccess(str);
			return;
		}

		log_debug("requestsession: %p %s has %d routes", q, qPrintable(host), route.targets.count());

		if(route.isNull())
		{
			state = WaitingForResponse;
			respondError(502, "Bad Gateway", QString("No route for host: %1").arg(host));
			return;
		}

		if(stats && !passthrough)
		{
			connectionRegistered = true;

			stats->addConnection(ridToString(rid), route.statsRoute(), StatsManager::Http, logicalPeerAddress, isHttps, false);
			stats->addActivity(route.statsRoute());
			stats->addRequestsReceived(1);
		}

		state = Prefetching;

		zhttpReqConnections.readyReadConnection = zhttpRequest->readyRead.connect(boost::bind(&Private::zhttpRequest_readyRead, this));
		processIncomingRequest();
	}

	void startRetry(int unreportedTime, int retrySeq)
	{
		trusted = ProxyUtil::checkTrustedClient("requestsession", q, requestData, defaultUpstreamKey);

		peerAddress = zhttpRequest->peerAddress();
		logicalPeerAddress = ProxyUtil::getLogicalAddress(requestData.headers, trusted ? xffTrustedRule : xffRule, peerAddress);

		zhttpReqConnections.pausedConnection = zhttpRequest->paused.connect(boost::bind(&Private::zhttpRequest_paused, this));
		zhttpReqConnections.errorConnection = zhttpRequest->error.connect(boost::bind(&Private::zhttpRequest_error, this));

		state = WaitingForResponse;

		bool isHttps = (requestData.uri.scheme() == "https");
		QString host = requestData.uri.host();

		QByteArray encPath = requestData.uri.path(QUrl::FullyEncoded).toUtf8();

		// look up the route
		if(!routeId.isEmpty() && !domainMap->isIdShared(routeId))
			route = domainMap->entry(routeId);
		else
			route = domainMap->entry(DomainMap::Http, isHttps, host, encPath);

		log_debug("requestsession: %p %s has %d routes", q, qPrintable(host), route.targets.count());

		if(route.isNull())
		{
			state = WaitingForResponse;
			respondError(502, "Bad Gateway", QString("No route for host: %1").arg(host));
			return;
		}

		if(stats)
		{
			if(retrySeq >= 0)
				stats->setRetrySeq(route.statsRoute(), retrySeq);

			connectionRegistered = true;

			int reportOffset = stats->connectionSendEnabled() ? -1 : qMax(unreportedTime, 0);

			stats->addConnection(ridToString(rid), route.statsRoute(), StatsManager::Http, logicalPeerAddress, isHttps, false, reportOffset);
			stats->addActivity(route.statsRoute());

			// note: we don't call addRequestsReceived here, because we're acting for an existing request
		}
	}

	void processIncomingRequest()
	{
		if(state == Prefetching)
		{
			if(prefetchSize > 0)
				in += zhttpRequest->readBody(prefetchSize - in.size());

			if(in.size() >= prefetchSize || zhttpRequest->isInputFinished())
			{
				// we've read enough body to start inspection

				zhttpReqConnections.readyReadConnection.disconnect();

				state = Inspecting;
				requestData.body = in.toByteArray();
				bool truncated = (!zhttpRequest->isInputFinished() || zhttpRequest->bytesAvailable() > 0);

				assert(!inspectRequest);

				if(inspectManager)
				{
					inspectRequest = new InspectRequest(inspectManager, this);

					if(inspectChecker->isInterfaceAvailable())
					{
						inspectFinishedConnection = inspectRequest->finished.connect(boost::bind(&Private::inspectRequest_finished, this));
						inspectChecker->watch(inspectRequest);
						inspectRequest->start(requestData, truncated, route.session, autoShare);
					}
					else
					{
						inspectChecker->watch(inspectRequest);
						inspectChecker->give(inspectRequest);
						inspectRequest->start(requestData, truncated, route.session, autoShare);
						inspectRequest = 0;
					}
				}

				if(!inspectRequest)
				{
					log_debug("inspect not available");
					QMetaObject::invokeMethod(this, "doInspectError", Qt::QueuedConnection);
				}
			}
		}
		else if(state == Receiving)
		{
			in += zhttpRequest->readBody(MAX_SHARED_REQUEST_BODY - in.size());

			if(in.size() >= MAX_SHARED_REQUEST_BODY || zhttpRequest->isInputFinished())
			{
				// we've read as much as we can for now. if there is still
				//   more to read, then the engine will notice this and
				//   disallow sharing before passing to proxysession. at that
				//   point, proxysession will read the remainder of the data

				zhttpReqConnections.readyReadConnection.disconnect();

				state = WaitingForResponse;
				requestData.body = in.take();
				q->inspected(idata);
			}
		}
		else if(state == ReceivingForAccept)
		{
			QByteArray buf = zhttpRequest->readBody();
			if(in.size() + buf.size() > MAX_ACCEPT_REQUEST_BODY)
			{
				respondError(413, "Request Entity Too Large", QString("Body must not exceed %1 bytes").arg(MAX_ACCEPT_REQUEST_BODY));
				return;
			}

			in += buf;

			if(zhttpRequest->isInputFinished())
			{
				if(acceptManager)
					zhttpRequest->pause();
				else
					respondCannotAccept();
			}
		}
	}

	void respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		q->startResponse(code, reason, headers);
		q->writeResponseBody(body);
		q->endResponseBody();
	}

	void respond(int code, const QString &status, const QByteArray &body)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		headers += HttpHeader("Content-Length", QByteArray::number(body.size()));

		respond(code, status.toUtf8(), headers, body);
	}

	void respondError(int code, const QString &status, const QString &errorString)
	{
		respond(code, status, errorString.toUtf8() + '\n');
	}

	void respondBadRequest(const QString &errorString)
	{
		respondError(400, "Bad Request", errorString);
	}

	void respondCannotAccept()
	{
		respondError(500, "Internal Server Error", "Accept service unavailable.");
	}

	void responseUpdate()
	{
		if(!pendingResponseUpdate)
		{
			pendingResponseUpdate = true;
			QMetaObject::invokeMethod(this, "doResponseUpdate", Qt::QueuedConnection);
		}
	}

	// returns null array on error
	QByteArray makeJsonpStart(int code, const QByteArray &reason, const HttpHeaders &headers)
	{
		QByteArray out = "/**/" + jsonpCallback + "(";

		if(jsonpExtendedResponse)
		{
			QByteArray reasonJson = serializeJsonString(QString::fromUtf8(reason));
			if(reasonJson.isNull())
				return QByteArray();

			QVariantMap vheaders;
			foreach(const HttpHeader h, headers)
			{
				if(!vheaders.contains(h.first))
					vheaders[h.first] = h.second;
			}

			QByteArray headersJson = QJsonDocument(QJsonObject::fromVariantMap(vheaders)).toJson(QJsonDocument::Compact);
			if(headersJson.isNull())
				return QByteArray();

			out += "{\"code\": " + QByteArray::number(code) + ", \"reason\": " + reasonJson + ", \"headers\": " + headersJson + ", \"body\": \"";
		}

		return out;
	}

	QByteArray makeJsonpBody(const QByteArray &buf)
	{
		if(jsonpExtendedResponse)
		{
			// FIXME: this assumes there isn't a partial character encoding
			QByteArray bodyJson = serializeJsonString(QString::fromUtf8(buf));
			if(bodyJson.isNull())
				return QByteArray();

			assert(bodyJson.size() >= 2);

			return bodyJson.mid(1, bodyJson.size() - 2);
		}
		else
			return buf;
	}

	QByteArray makeJsonpEnd()
	{
		if(jsonpExtendedResponse)
			return QByteArray("\"});\n");
		else
			return QByteArray(");\n");
	}

	// return true if jsonp applied
	bool tryApplyJsonp(const DomainMap::JsonpConfig &config, bool *ok, QString *errorMessage)
	{
		*ok = true;

		// must be a GET
		if(requestData.method != "GET")
			return false;

		QString callbackParam = QString::fromUtf8(config.callbackParam);
		if(callbackParam.isEmpty())
			callbackParam = "callback";

		QString bodyParam;
		if(!config.bodyParam.isEmpty())
			bodyParam = QString::fromUtf8(config.bodyParam);

		QUrl uri = requestData.uri;
		QUrlQuery query(uri);

		// two ways to activate JSON-P:
		//   1) callback param present
		//   2) default callback specified in configuration and body param present
		if(!query.hasQueryItem(callbackParam) &&
			(config.defaultCallback.isEmpty() || bodyParam.isEmpty() || !query.hasQueryItem(bodyParam)))
		{
			return false;
		}

		QByteArray callback;
		if(query.hasQueryItem(callbackParam))
		{
			callback = parsePercentEncoding(query.queryItemValue(callbackParam, QUrl::FullyEncoded).toUtf8());
			if(callback.isEmpty())
			{
				log_debug("requestsession: id=%s invalid callback parameter, rejecting", rid.second.data());
				*ok = false;
				*errorMessage = "Invalid callback parameter.";
				return false;
			}

			query.removeAllQueryItems(callbackParam);
		}
		else
			callback = config.defaultCallback;

		QString method;
		if(query.hasQueryItem("_method"))
		{
			method = QString::fromLatin1(parsePercentEncoding(query.queryItemValue("_method", QUrl::FullyEncoded).toUtf8()));
			if(!validMethod(method))
			{
				log_debug("requestsession: id=%s invalid _method parameter, rejecting", rid.second.data());
				*ok = false;
				*errorMessage = "Invalid _method parameter.";
				return false;
			}

			query.removeAllQueryItems("_method");
		}

		HttpHeaders headers;
		if(query.hasQueryItem("_headers"))
		{
			QJsonParseError e;
			QJsonDocument doc = QJsonDocument::fromJson(parsePercentEncoding(query.queryItemValue("_headers", QUrl::FullyEncoded).toUtf8()), &e);
			if(e.error != QJsonParseError::NoError || !doc.isObject())
			{
				log_debug("requestsession: id=%s invalid _headers parameter, rejecting", rid.second.data());
				*ok = false;
				*errorMessage = "Invalid _headers parameter.";
				return false;
			}

			QVariantMap headersMap = doc.object().toVariantMap();

			QMapIterator<QString, QVariant> vit(headersMap);
			while(vit.hasNext())
			{
				vit.next();

				if(typeId(vit.value()) != QMetaType::QString)
				{
					log_debug("requestsession: id=%s invalid _headers parameter, rejecting", rid.second.data());
					*ok = false;
					*errorMessage = "Invalid _headers parameter.";
					return false;
				}

				QByteArray key = vit.key().toUtf8();

				// ignore some headers that we explicitly set later on
				if(qstricmp(key.data(), "host") == 0)
					continue;
				if(qstricmp(key.data(), "accept") == 0)
					continue;

				headers += HttpHeader(key, vit.value().toString().toUtf8());
			}

			query.removeAllQueryItems("_headers");
		}

		QByteArray body;
		if(!bodyParam.isEmpty())
		{
			if(query.hasQueryItem(bodyParam))
			{
				body = parsePercentEncoding(query.queryItemValue(bodyParam, QUrl::FullyEncoded).toUtf8());
				if(body.isNull())
				{
					log_debug("requestsession: id=%s invalid body parameter, rejecting", rid.second.data());
					*ok = false;
					*errorMessage = "Invalid body parameter.";
					return false;
				}

				headers.removeAll("Content-Type");
				headers += HttpHeader("Content-Type", "application/json");

				query.removeAllQueryItems(bodyParam);
			}
		}
		else
		{
			if(query.hasQueryItem("_body"))
			{
				body = parsePercentEncoding(query.queryItemValue("_body").toUtf8());
				if(body.isNull())
				{
					log_debug("requestsession: id=%s invalid _body parameter, rejecting", rid.second.data());
					*ok = false;
					*errorMessage = "Invalid _body parameter.";
					return false;
				}

				query.removeAllQueryItems("_body");
			}
		}

		uri.setQuery(query);

		// if we have no query items anymore, strip the '?'
		if(query.isEmpty())
		{
			QByteArray tmp = uri.toEncoded();
			if(tmp.length() > 0 && tmp[tmp.length() - 1] == '?')
			{
				tmp.truncate(tmp.length() - 1);
				uri = QUrl::fromEncoded(tmp, QUrl::StrictMode);
			}
		}

		if(method.isEmpty())
			method = config.defaultMethod;

		requestData.method = method;

		requestData.uri = uri;

		QByteArray hostHeader = uri.host().toUtf8();
		if(uri.port() != -1)
			hostHeader += ':' + QByteArray::number(uri.port());
		headers += HttpHeader("Host", hostHeader);

		headers += HttpHeader("Accept", "*/*");

		// carry over the rest of the headers
		foreach(const HttpHeader &h, requestData.headers)
		{
			if(qstricmp(h.first.data(), "host") == 0)
				continue;
			if(qstricmp(h.first.data(), "accept") == 0)
				continue;

			headers += h;
		}

		requestData.headers = headers;
		in += body;

		jsonpCallback = callback;
		jsonpExtendedResponse = (config.mode == DomainMap::JsonpConfig::Extended);

		return true;
	}

public:
	void zhttpRequest_readyRead()
	{
		processIncomingRequest();
	}

	void zhttpRequest_bytesWritten(int count)
	{
		QPointer<QObject> self = this;

		if(!jsonpCallback.isEmpty())
		{
			int actual = jsonpTracker.finished(count);
			if(actual > 0)
				q->bytesWritten(actual);
		}
		else
			q->bytesWritten(count);

		if(!self)
			return;

		if(zhttpRequest->isFinished())
		{
			cleanup();
			q->finished();
		}
	}

	void zhttpRequest_paused()
	{
		if(state == ReceivingForAccept)
		{
			ZhttpRequest::ServerState ss = zhttpRequest->serverState();

			AcceptData adata;

			AcceptData::Request areq;
			areq.rid = rid;
			areq.https = requestData.uri.scheme() == "https";
			areq.peerAddress = peerAddress;
			areq.logicalPeerAddress = logicalPeerAddress;
			areq.debug = debug;
			areq.autoCrossOrigin = autoCrossOrigin;
			areq.jsonpCallback = jsonpCallback;
			areq.jsonpExtendedResponse = jsonpExtendedResponse;
			areq.inSeq = ss.inSeq;
			areq.outSeq = ss.outSeq;
			areq.outCredits = ss.outCredits;
			areq.userData = ss.userData;
			adata.requests += areq;

			adata.requestData = requestData;
			adata.requestData.body = in.take();

			adata.haveInspectData = true;
			adata.inspectData = idata;

			adata.route = route.id;
			adata.channelPrefix = route.prefix;

			acceptRequest = new AcceptRequest(acceptManager, this);
			acceptFinishedConnection = acceptRequest->finished.connect(boost::bind(&Private::acceptRequest_finished, this));
			acceptRequest->start(adata);
		}
		else
		{
			q->paused();
		}
	}

	void zhttpRequest_error()
	{
		log_debug("requestsession: request error id=%s", rid.second.data());
		cleanup();
		q->finished();
	}

	void inspectRequest_finished()
	{
		if(!inspectRequest->success())
		{
			inspectFinishedConnection.disconnect();
			inspectChecker->give(inspectRequest);
			inspectRequest = 0;

			doInspectError();
			return;
		}

		idata = inspectRequest->result();

		inspectFinishedConnection.disconnect();
		inspectChecker->give(inspectRequest);
		inspectRequest = 0;

		if(!idata.doProxy)
		{
			state = ReceivingForAccept;

			// successful inspect indicated we should not proxy. in that case,
			//   collect the body and accept
			zhttpReqConnections.readyReadConnection = zhttpRequest->readyRead.connect(boost::bind(&Private::zhttpRequest_readyRead, this));
			processIncomingRequest();
		}
		else
		{
			if(!idata.sharingKey.isEmpty())
			{
				// a request can only be shared if we've read the entire
				//   request body, so let's try to read it now
				state = Receiving;

				zhttpReqConnections.readyReadConnection = zhttpRequest->readyRead.connect(boost::bind(&Private::zhttpRequest_readyRead, this));
				processIncomingRequest();
			}
			else
			{
				state = WaitingForResponse;
				requestData.body = in.take();
				q->inspected(idata);
			}
		}
	}

	void acceptRequest_finished()
	{
		if(acceptRequest->success())
		{
			AcceptRequest::ResponseData rdata = acceptRequest->result();

			acceptFinishedConnection.disconnect();
			delete acceptRequest;
			acceptRequest = 0;

			if(rdata.accepted)
			{
				accepted = true;

				// the request was paused, so deleting it will leave the peer session active
				zhttpReqConnections = ZhttpReqConnections();
				delete zhttpRequest;
				zhttpRequest = 0;

				cleanup();
				q->finishedByAccept();
			}
			else
			{
				if(rdata.response.code != -1)
				{
					zhttpRequest->resume();
					respond(rdata.response.code, rdata.response.reason, rdata.response.headers, rdata.response.body);
				}
				else
				{
					zhttpRequest->resume();
					respondCannotAccept();
				}
			}
		}
		else
		{
			acceptFinishedConnection.disconnect();
			delete acceptRequest;
			acceptRequest = 0;

			zhttpRequest->resume();
			respondCannotAccept();
		}
	}

public slots:
	void doResponseUpdate()
	{
		pendingResponseUpdate = false;

		if(state == RespondingStart)
		{
			state = Responding;

			if(!jsonpCallback.isEmpty())
			{
				HttpHeaders headers;

				if(responseBodyFinished)
				{
					QByteArray bodyRawBuf = out.take();

					if(!jsonpExtendedResponse)
					{
						// trim any trailing newline before we wrap in a function call
						if(bodyRawBuf.endsWith("\r\n"))
							bodyRawBuf.truncate(bodyRawBuf.size() - 2);
						else if(bodyRawBuf.endsWith("\n"))
							bodyRawBuf.truncate(bodyRawBuf.size() - 1);
					}

					QByteArray startBuf = makeJsonpStart(responseData.code, responseData.reason, responseData.headers);
					QByteArray bodyBuf;
					QByteArray endBuf = makeJsonpEnd();
					if(!startBuf.isNull())
						bodyBuf = makeJsonpBody(bodyRawBuf);

					if(startBuf.isNull() || bodyBuf.isNull())
					{
						state = RespondingInternal;

						QByteArray body = "Upstream response could not be JSON-P encoded.\n";
						headers += HttpHeader("Content-Type", "text/plain");
						headers += HttpHeader("Content-Length", QByteArray::number(body.size()));
						zhttpRequest->beginResponse(500, "Internal Server Error", headers);
						zhttpRequest->writeBody(body);
						responseBodySize += body.size();
						zhttpRequest->endBody();
						q->errorResponding();
						return;
					}

					QByteArray buf = startBuf + bodyBuf + endBuf;

					headers += HttpHeader("Content-Type", "application/javascript");
					headers += HttpHeader("Content-Length", QByteArray::number(buf.size()));

					// mirror headers in the wrapping response
					foreach(const HttpHeader &h, responseData.headers)
					{
						foreach(const QByteArray &eh, jsonpExtractableHeaders)
						{
							if(qstricmp(h.first.data(), eh.data()) == 0)
							{
								headers += h;
								break;
							}
						}
					}

					zhttpReqConnections.bytesWrittenConnection = zhttpRequest->bytesWritten.connect(boost::bind(&Private::zhttpRequest_bytesWritten, this, boost::placeholders::_1));

					zhttpRequest->beginResponse(200, "OK", headers);

					jsonpTracker.addPlain(bodyRawBuf.size());
					jsonpTracker.specifyEncoded(buf.size(), bodyRawBuf.size());

					zhttpRequest->writeBody(buf);
					responseBodySize += buf.size();
					zhttpRequest->endBody();
					return;
				}

				QByteArray buf = makeJsonpStart(responseData.code, responseData.reason, responseData.headers);
				if(buf.isNull())
				{
					state = RespondingInternal;

					QByteArray body = "Upstream response could not be JSON-P encoded.\n";
					headers += HttpHeader("Content-Type", "text/plain");
					headers += HttpHeader("Content-Length", QByteArray::number(body.size()));
					zhttpRequest->beginResponse(500, "Internal Server Error", headers);
					zhttpRequest->writeBody(body);
					responseBodySize += body.size();
					zhttpRequest->endBody();
					q->errorResponding();
					return;
				}

				headers += HttpHeader("Content-Type", "application/javascript");
				headers += HttpHeader("Transfer-Encoding", "chunked");

				zhttpReqConnections.bytesWrittenConnection = zhttpRequest->bytesWritten.connect(boost::bind(&Private::zhttpRequest_bytesWritten, this, boost::placeholders::_1));

				zhttpRequest->beginResponse(200, "OK", headers);

				jsonpTracker.specifyEncoded(buf.size(), 0);

				zhttpRequest->writeBody(buf);
				responseBodySize += buf.size();
			}
			else
			{
				if(autoCrossOrigin)
					Cors::applyCorsHeaders(requestData.headers, &responseData.headers);

				zhttpReqConnections.bytesWrittenConnection = zhttpRequest->bytesWritten.connect(boost::bind(&Private::zhttpRequest_bytesWritten, this, boost::placeholders::_1));

				zhttpRequest->beginResponse(responseData.code, responseData.reason, responseData.headers);
			}
		}

		if(!out.isEmpty())
		{
			if(!jsonpCallback.isEmpty())
			{
				QByteArray bodyRawBuf = out.take();

				if(!jsonpExtendedResponse)
				{
					if(responseBodyFinished)
					{
						// trim any trailing newline before we wrap in a function call
						if(bodyRawBuf.endsWith("\r\n"))
							bodyRawBuf.truncate(bodyRawBuf.size() - 2);
						else if(bodyRawBuf.endsWith("\n"))
							bodyRawBuf.truncate(bodyRawBuf.size() - 1);
					}
					else
					{
						// response isn't finished. keep any trailing newline in the output buffer
						if(bodyRawBuf.endsWith("\r\n"))
						{
							bodyRawBuf.truncate(bodyRawBuf.size() - 2);
							out += QByteArray("\r\n");
						}
						else if(bodyRawBuf.endsWith("\n"))
						{
							bodyRawBuf.truncate(bodyRawBuf.size() - 1);
							out += QByteArray("\n");
						}
					}
				}

				QByteArray buf = makeJsonpBody(bodyRawBuf);
				if(buf.isNull())
				{
					state = RespondingInternal;

					log_warning("requestsession: id=%s upstream response could not be JSON-P encoded", rid.second.data());

					// if we error while streaming, all we can do is give up
					zhttpRequest->endBody();
					q->errorResponding();
					return;
				}

				jsonpTracker.addPlain(bodyRawBuf.size());
				jsonpTracker.specifyEncoded(buf.size(), bodyRawBuf.size());

				zhttpRequest->writeBody(buf);
				responseBodySize += buf.size();
			}
			else
			{
				QByteArray buf = out.take();
				zhttpRequest->writeBody(buf);
				responseBodySize += buf.size();
			}
		}

		if(responseBodyFinished)
		{
			assert(!needPause);

			if(!jsonpCallback.isEmpty())
			{
				QByteArray buf = makeJsonpEnd();
				jsonpTracker.specifyEncoded(buf.size(), 0);
				zhttpRequest->writeBody(buf);
				responseBodySize += buf.size();
			}

			zhttpRequest->endBody();
		}
		else if(needPause)
		{
			needPause = false;
			zhttpRequest->pause();
		}
	}

	void respondSuccess(const QString &message)
	{
		respond(200, "OK", message.toUtf8() + '\n');
	}

	void doInspectError()
	{
		state = WaitingForResponse;
		q->inspectError();
	}

	void doFinished()
	{
		q->finished();
	}
};

RequestSession::RequestSession(int workerId, StatsManager *stats, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, workerId, 0, 0, 0, 0, 0, stats);
}

RequestSession::RequestSession(int workerId, DomainMap *domainMap, SockJsManager *sockJsManager, ZrpcManager *inspectManager, ZrpcChecker *inspectChecker, ZrpcManager *acceptManager, StatsManager *stats, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, workerId, domainMap, sockJsManager, inspectManager, inspectChecker, acceptManager, stats);
}

RequestSession::~RequestSession()
{
	delete d;
}

bool RequestSession::isRetry() const
{
	return d->isRetry;
}

bool RequestSession::isHttps() const
{
	return d->requestData.uri.scheme() == "https";
}

bool RequestSession::isSockJs() const
{
	return d->isSockJs;
};

bool RequestSession::trusted() const
{
	return d->trusted;
}

QHostAddress RequestSession::peerAddress() const
{
	return d->peerAddress;
}

QHostAddress RequestSession::logicalPeerAddress() const
{
	return d->logicalPeerAddress;
}

ZhttpRequest::Rid RequestSession::rid() const
{
	return d->rid;
}

HttpRequestData RequestSession::requestData() const
{
	return d->requestData;
}

HttpResponseData RequestSession::responseData() const
{
	return d->responseData;
}

int RequestSession::responseBodySize() const
{
	return d->responseBodySize;
}

bool RequestSession::debugEnabled() const
{
	return d->debug;
}

bool RequestSession::autoCrossOrigin() const
{
	return d->autoCrossOrigin;
}

QByteArray RequestSession::jsonpCallback() const
{
	return d->jsonpCallback;
}

bool RequestSession::jsonpExtendedResponse() const
{
	return d->jsonpExtendedResponse;
}

bool RequestSession::haveCompleteRequestBody() const
{
	return (d->zhttpRequest->isInputFinished() && d->zhttpRequest->bytesAvailable() == 0);
}

DomainMap::Entry RequestSession::route() const
{
	return d->route;
}

ZhttpRequest *RequestSession::request()
{
	return d->zhttpRequest;
}

void RequestSession::setDebugEnabled(bool enabled)
{
	d->debug = enabled;
}

void RequestSession::setAutoCrossOrigin(bool enabled)
{
	d->autoCrossOrigin = enabled;
}

void RequestSession::setPrefetchSize(int size)
{
	d->prefetchSize = size;
}

void RequestSession::setRoute(const DomainMap::Entry &route)
{
	d->route = route;
}

void RequestSession::setRouteId(const QString &routeId)
{
	d->routeId = routeId;
}

void RequestSession::setAutoShare(bool enabled)
{
	d->autoShare = enabled;
}

void RequestSession::setAccepted(bool enabled)
{
	d->accepted = enabled;
}

void RequestSession::setDefaultUpstreamKey(const Jwt::DecodingKey &key)
{
	d->defaultUpstreamKey = key;
}

void RequestSession::setXffRules(const XffRule &untrusted, const XffRule &trusted)
{
	d->xffRule = untrusted;
	d->xffTrustedRule = trusted;
}

void RequestSession::start(ZhttpRequest *req)
{
	d->start(req);
}

void RequestSession::startRetry(ZhttpRequest *req, bool debug, bool autoCrossOrigin, const QByteArray &jsonpCallback, bool jsonpExtendedResponse, int unreportedTime, int retrySeq)
{
	d->isRetry = true;
	d->zhttpRequest = req;
	d->rid = req->rid();
	d->debug = debug;
	d->autoCrossOrigin = autoCrossOrigin;
	d->jsonpCallback = jsonpCallback;
	d->jsonpExtendedResponse = jsonpExtendedResponse;
	d->requestData.method = req->requestMethod();
	d->requestData.uri = req->requestUri();
	d->requestData.headers = req->requestHeaders();
	d->requestData.body = req->readBody();

	d->startRetry(unreportedTime, retrySeq);
}

void RequestSession::pause()
{
	assert(!d->responseBodyFinished);
	d->needPause = true;

	d->responseUpdate();
}

void RequestSession::resume()
{
	d->zhttpRequest->resume();
}

void RequestSession::startResponse(int code, const QByteArray &reason, const HttpHeaders &headers)
{
	assert(d->state == Private::ReceivingForAccept || d->state == Private::WaitingForResponse);

	headerBytesSent(ZhttpManager::estimateResponseHeaderBytes(code, reason, headers));

	d->state = Private::RespondingStart;
	d->responseData.code = code;
	d->responseData.reason = reason;
	d->responseData.headers = headers;

	d->responseUpdate();
}

void RequestSession::writeResponseBody(const QByteArray &body)
{
	assert(d->state == Private::RespondingStart || d->state == Private::Responding);
	assert(!d->responseBodyFinished);

	bodyBytesSent(body.size());

	d->out += body;
	d->responseUpdate();
}

void RequestSession::endResponseBody()
{
	assert(d->state == Private::RespondingStart || d->state == Private::Responding);
	assert(!d->responseBodyFinished);

	d->responseBodyFinished = true;
	d->responseUpdate();
}

void RequestSession::respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
{
	d->respond(code, reason, headers, body);
}

void RequestSession::respondError(int code, const QString &reason, const QString &errorString)
{
	d->respondError(code, reason, errorString);
}

void RequestSession::respondCannotAccept()
{
	d->respondCannotAccept();
}

int RequestSession::unregisterConnection()
{
	if(!d->connectionRegistered)
		return 0;

	d->connectionRegistered = false;

	QByteArray cid = ridToString(d->rid);
	return d->stats->removeConnection(cid, false);
}

#include "requestsession.moc"
