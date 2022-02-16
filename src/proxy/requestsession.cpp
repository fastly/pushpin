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
#include "bufferlist.h"
#include "log.h"
#include "layertracker.h"
#include "sockjsmanager.h"
#include "inspectdata.h"
#include "acceptdata.h"
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

	RequestSession *q;
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
	QByteArray defaultUpstreamKey;
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

	Private(RequestSession *_q, DomainMap *_domainMap = 0, SockJsManager *_sockJsManager = 0, ZrpcManager *_inspectManager = 0, ZrpcChecker *_inspectChecker = 0, ZrpcManager *_acceptManager = 0, StatsManager *_stats = 0) :
		QObject(_q),
		q(_q),
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
			delete zhttpRequest;
			zhttpRequest = 0;
		}

		if(inspectRequest)
		{
			inspectRequest->disconnect(this);
			inspectChecker->give(inspectRequest);
			inspectRequest = 0;
		}

		if(stats && connectionRegistered)
		{
			QByteArray cid = ridToString(rid);

			// refresh before remove, to ensure transition
			if(accepted)
				stats->refreshConnection(cid);

			// linger if accepted
			stats->removeConnection(cid, accepted);
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

		log_debug("IN id=%s, %s %s", rid.second.data(), qPrintable(requestData.method), requestData.uri.toEncoded().data());

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
				QMetaObject::invokeMethod(q, "finished", Qt::QueuedConnection);
				return;
			}
		}

		connect(zhttpRequest, &ZhttpRequest::paused, this, &Private::zhttpRequest_paused);
		connect(zhttpRequest, &ZhttpRequest::error, this, &Private::zhttpRequest_error);

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
			"Copyright (C) 2012-2017 Fanout, Inc.\n"
			"\n"
			"Pushpin is free software: you can redistribute it and/or modify it under\n"
			"the terms of the GNU Affero General Public License as published by the Free\n"
			"Software Foundation, either version 3 of the License, or (at your option)\n"
			"any later version.\n"
			"\n"
			"Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY\n"
			"WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS\n"
			"FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for\n"
			"more details.\n"
			"\n"
			"You should have received a copy of the GNU Affero General Public License\n"
			"along with this program. If not, see <http://www.gnu.org/licenses/>.\n";

			state = WaitingForResponse;
			respondSuccess(str);
			return;
		}

		if(route.isNull())
		{
			log_warning("requestsession: %p %s has 0 routes", q, qPrintable(host));

			state = WaitingForResponse;
			respondError(502, "Bad Gateway", QString("No route for host: %1").arg(host));
			return;
		}

		log_debug("requestsession: %p %s has %d routes", q, qPrintable(host), route.targets.count());

		if(stats && !passthrough)
		{
			connectionRegistered = true;

			stats->addConnection(ridToString(rid), route.statsRoute(), StatsManager::Http, logicalPeerAddress, isHttps, false);
			stats->addActivity(route.statsRoute());
			stats->addRequestsReceived(1);
		}

		state = Prefetching;

		connect(zhttpRequest, &ZhttpRequest::readyRead, this, &Private::zhttpRequest_readyRead);
		processIncomingRequest();
	}

	void startRetry()
	{
		trusted = ProxyUtil::checkTrustedClient("requestsession", q, requestData, defaultUpstreamKey);

		peerAddress = zhttpRequest->peerAddress();
		logicalPeerAddress = ProxyUtil::getLogicalAddress(requestData.headers, trusted ? xffTrustedRule : xffRule, peerAddress);

		connect(zhttpRequest, &ZhttpRequest::error, this, &Private::zhttpRequest_error);
		connect(zhttpRequest, &ZhttpRequest::paused, this, &Private::zhttpRequest_paused);

		state = WaitingForResponse;

		bool isHttps = (requestData.uri.scheme() == "https");
		QString host = requestData.uri.host();

		QByteArray encPath = requestData.uri.path(QUrl::FullyEncoded).toUtf8();

		// look up the route
		if(!routeId.isEmpty() && !domainMap->isIdShared(routeId))
			route = domainMap->entry(routeId);
		else
			route = domainMap->entry(DomainMap::Http, isHttps, host, encPath);

		if(route.isNull())
		{
			log_warning("requestsession: %p %s has 0 routes", q, qPrintable(host));

			state = WaitingForResponse;
			respondError(502, "Bad Gateway", QString("No route for host: %1").arg(host));
			return;
		}

		log_debug("proxysession: %p %s has %d routes", q, qPrintable(host), route.targets.count());

		if(stats)
		{
			connectionRegistered = true;

			stats->addConnection(ridToString(rid), route.statsRoute(), StatsManager::Http, logicalPeerAddress, isHttps, false);
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

				disconnect(zhttpRequest, &ZhttpRequest::readyRead, this, &Private::zhttpRequest_readyRead);

				state = Inspecting;
				requestData.body = in.toByteArray();
				bool truncated = (!zhttpRequest->isInputFinished() || zhttpRequest->bytesAvailable() > 0);

				assert(!inspectRequest);

				if(inspectManager)
				{
					inspectRequest = new InspectRequest(inspectManager, this);

					if(inspectChecker->isInterfaceAvailable())
					{
						connect(inspectRequest, &InspectRequest::finished, this, &Private::inspectRequest_finished);
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

				disconnect(zhttpRequest, &ZhttpRequest::readyRead, this, &Private::zhttpRequest_readyRead);

				state = WaitingForResponse;
				requestData.body = in.take();
				emit q->inspected(idata);
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

				if(vit.value().type() != QVariant::String)
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

public slots:
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
				emit q->bytesWritten(actual);
		}
		else
			emit q->bytesWritten(count);

		if(!self)
			return;

		if(zhttpRequest->isFinished())
		{
			cleanup();
			emit q->finished();
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
			connect(acceptRequest, &AcceptRequest::finished, this, &Private::acceptRequest_finished);
			acceptRequest->start(adata);
		}
		else
		{
			emit q->paused();
		}
	}

	void zhttpRequest_error()
	{
		log_debug("requestsession: request error id=%s", rid.second.data());
		cleanup();
		emit q->finished();
	}

	void inspectRequest_finished()
	{
		if(!inspectRequest->success())
		{
			inspectRequest->disconnect(this);
			inspectChecker->give(inspectRequest);
			inspectRequest = 0;

			doInspectError();
			return;
		}

		idata = inspectRequest->result();

		inspectRequest->disconnect(this);
		inspectChecker->give(inspectRequest);
		inspectRequest = 0;

		if(!idata.doProxy)
		{
			state = ReceivingForAccept;

			// successful inspect indicated we should not proxy. in that case,
			//   collect the body and accept
			connect(zhttpRequest, &ZhttpRequest::readyRead, this, &Private::zhttpRequest_readyRead);
			processIncomingRequest();
		}
		else
		{
			if(!idata.sharingKey.isEmpty())
			{
				// a request can only be shared if we've read the entire
				//   request body, so let's try to read it now
				state = Receiving;

				connect(zhttpRequest, &ZhttpRequest::readyRead, this, &Private::zhttpRequest_readyRead);
				processIncomingRequest();
			}
			else
			{
				state = WaitingForResponse;
				requestData.body = in.take();
				emit q->inspected(idata);
			}
		}
	}

	void acceptRequest_finished()
	{
		if(acceptRequest->success())
		{
			AcceptRequest::ResponseData rdata = acceptRequest->result();

			delete acceptRequest;
			acceptRequest = 0;

			if(rdata.accepted)
			{
				accepted = true;

				// the request was paused, so deleting it will leave the peer session active
				delete zhttpRequest;
				zhttpRequest = 0;

				cleanup();
				emit q->finishedByAccept();
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
			delete acceptRequest;
			acceptRequest = 0;

			zhttpRequest->resume();
			respondCannotAccept();
		}
	}

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
						emit q->errorResponding();
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

					connect(zhttpRequest, &ZhttpRequest::bytesWritten, this, &Private::zhttpRequest_bytesWritten);

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
					emit q->errorResponding();
					return;
				}

				headers += HttpHeader("Content-Type", "application/javascript");
				headers += HttpHeader("Transfer-Encoding", "chunked");

				connect(zhttpRequest, &ZhttpRequest::bytesWritten, this, &Private::zhttpRequest_bytesWritten);

				zhttpRequest->beginResponse(200, "OK", headers);

				jsonpTracker.specifyEncoded(buf.size(), 0);

				zhttpRequest->writeBody(buf);
				responseBodySize += buf.size();
			}
			else
			{
				if(autoCrossOrigin)
					Cors::applyCorsHeaders(requestData.headers, &responseData.headers);

				connect(zhttpRequest, &ZhttpRequest::bytesWritten, this, &Private::zhttpRequest_bytesWritten);

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
					emit q->errorResponding();
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
		emit q->inspectError();
	}
};

RequestSession::RequestSession(StatsManager *stats, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, 0, 0, 0, 0, 0, stats);
}

RequestSession::RequestSession(DomainMap *domainMap, SockJsManager *sockJsManager, ZrpcManager *inspectManager, ZrpcChecker *inspectChecker, ZrpcManager *acceptManager, StatsManager *stats, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, domainMap, sockJsManager, inspectManager, inspectChecker, acceptManager, stats);
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

void RequestSession::setDefaultUpstreamKey(const QByteArray &key)
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

void RequestSession::startRetry(ZhttpRequest *req, bool debug, bool autoCrossOrigin, const QByteArray &jsonpCallback, bool jsonpExtendedResponse)
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

	d->startRetry();
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

#include "requestsession.moc"
