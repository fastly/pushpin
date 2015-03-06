/*
 * Copyright (C) 2012-2015 Fanout, Inc.
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

#include "requestsession.h"

#include <assert.h>
#include <QPointer>
#include <QUrl>
#include <QHostAddress>
#include <qjson/parser.h>
#include <qjson/serializer.h>
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

#define MAX_PREFETCH_REQUEST_BODY 10000
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

static bool validMethod(const QByteArray &in)
{
	if(in.isEmpty())
		return false;

	for(int n = 0; n < in.size(); ++n)
	{
		unsigned char c = (unsigned char)in[n];
		if(c <= 0x20)
			return false;
	}

	return true;
}

static bool isSimpleHeader(const QByteArray &in)
{
	return (qstricmp(in.data(), "Cache-Control") == 0 ||
		qstricmp(in.data(), "Content-Language") == 0 ||
		qstricmp(in.data(), "Content-Length") == 0 ||
		qstricmp(in.data(), "Content-Type") == 0 ||
		qstricmp(in.data(), "Expires") == 0 ||
		qstricmp(in.data(), "Last-Modified") == 0 ||
		qstricmp(in.data(), "Pragma") == 0);
}

static bool headerNamesContains(const QList<QByteArray> &names, const QByteArray &name)
{
	foreach(const QByteArray &i, names)
	{
		if(qstricmp(name.data(), i.data()) == 0)
			return true;
	}

	return false;
}

static bool headerNameStartsWith(const QByteArray &name, const char *value)
{
	return (qstrnicmp(name.data(), value, qstrlen(value)) == 0);
}

static void applyCorsHeaders(const HttpHeaders &requestHeaders, HttpHeaders *responseHeaders)
{
	if(!responseHeaders->contains("Access-Control-Allow-Methods"))
	{
		QByteArray method = requestHeaders.get("Access-Control-Request-Method");

		if(!method.isEmpty())
			*responseHeaders += HttpHeader("Access-Control-Allow-Methods", method);
		else
			*responseHeaders += HttpHeader("Access-Control-Allow-Methods", "OPTIONS, HEAD, GET, POST, PUT, DELETE");
	}

	if(!responseHeaders->contains("Access-Control-Allow-Headers"))
	{
		QList<QByteArray> allowHeaders;
		foreach(const QByteArray &h, requestHeaders.getAll("Access-Control-Request-Headers", true))
		{
			if(!h.isEmpty())
				allowHeaders += h;
		}

		if(!allowHeaders.isEmpty())
			*responseHeaders += HttpHeader("Access-Control-Allow-Headers", HttpHeaders::join(allowHeaders));
	}

	if(!responseHeaders->contains("Access-Control-Expose-Headers"))
	{
		QList<QByteArray> exposeHeaders;
		foreach(const HttpHeader &h, *responseHeaders)
		{
			if(!isSimpleHeader(h.first) && !headerNameStartsWith(h.first, "Access-Control-") && !headerNamesContains(exposeHeaders, h.first))
				exposeHeaders += h.first;
		}

		if(!exposeHeaders.isEmpty())
			*responseHeaders += HttpHeader("Access-Control-Expose-Headers", HttpHeaders::join(exposeHeaders));
	}

	if(!responseHeaders->contains("Access-Control-Allow-Credentials"))
		*responseHeaders += HttpHeader("Access-Control-Allow-Credentials", "true");

	if(!responseHeaders->contains("Access-Control-Allow-Origin"))
	{
		QByteArray origin = requestHeaders.get("Origin");

		if(origin.isEmpty())
			origin = "*";

		*responseHeaders += HttpHeader("Access-Control-Allow-Origin", origin);
	}
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
	ZhttpRequest *zhttpRequest;
	HttpRequestData requestData;
	DomainMap::Entry route;
	bool autoCrossOrigin;
	InspectRequest *inspectRequest;
	InspectData idata;
	AcceptRequest *acceptRequest;
	BufferList in;
	QByteArray jsonpCallback;
	bool jsonpExtendedResponse;
	HttpResponseData responseData;
	BufferList out;
	bool responseBodyFinished;
	bool pendingResponseUpdate;
	LayerTracker jsonpTracker;
	bool isRetry;
	QList<QByteArray> jsonpExtractableHeaders;

	Private(RequestSession *_q, DomainMap *_domainMap, SockJsManager *_sockJsManager, ZrpcManager *_inspectManager, ZrpcChecker *_inspectChecker, ZrpcManager *_acceptManager) :
		QObject(_q),
		q(_q),
		state(Stopped),
		domainMap(_domainMap),
		sockJsManager(_sockJsManager),
		inspectManager(_inspectManager),
		inspectChecker(_inspectChecker),
		acceptManager(_acceptManager),
		zhttpRequest(0),
		autoCrossOrigin(false),
		inspectRequest(0),
		acceptRequest(0),
		jsonpExtendedResponse(false),
		responseBodyFinished(false),
		pendingResponseUpdate(false),
		isRetry(false)
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

		state = Stopped;
	}

	void start(ZhttpRequest *req)
	{
		zhttpRequest = req;
		rid = req->rid();

		requestData.method = req->requestMethod();
		requestData.uri = req->requestUri();
		requestData.headers = req->requestHeaders();

		log_info("IN id=%s, %s %s", rid.second.data(), qPrintable(requestData.method), requestData.uri.toEncoded().data());

		bool isHttps = (requestData.uri.scheme() == "https");
		QString host = requestData.uri.host();

		// look up the route
		route = domainMap->entry(DomainMap::Http, isHttps, host, requestData.uri.encodedPath());

		// before we do anything else, see if this is a sockjs request
		if(!route.isNull() && !route.sockJsPath.isEmpty() && requestData.uri.encodedPath().startsWith(route.sockJsPath))
		{
			sockJsManager->giveRequest(zhttpRequest, route.sockJsPath.length(), route.sockJsAsPath, route);
			zhttpRequest = 0;
			QMetaObject::invokeMethod(q, "finished", Qt::QueuedConnection);
			return;
		}

		connect(zhttpRequest, SIGNAL(error()), SLOT(zhttpRequest_error()));
		connect(zhttpRequest, SIGNAL(paused()), SLOT(zhttpRequest_paused()));

		if(autoCrossOrigin || (!route.isNull() && route.autoCrossOrigin))
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
			"Copyright (C) 2012-2015 Fanout, Inc.\n"
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

		state = Prefetching;

		connect(zhttpRequest, SIGNAL(readyRead()), SLOT(zhttpRequest_readyRead()));
		processIncomingRequest();
	}

	void startRetry()
	{
		connect(zhttpRequest, SIGNAL(error()), SLOT(zhttpRequest_error()));
		connect(zhttpRequest, SIGNAL(paused()), SLOT(zhttpRequest_paused()));

		state = WaitingForResponse;

		bool isHttps = (requestData.uri.scheme() == "https");
		QString host = requestData.uri.host();

		// look up the route
		route = domainMap->entry(DomainMap::Http, isHttps, host, requestData.uri.encodedPath());
		if(route.isNull())
		{
			log_warning("requestsession: %p %s has 0 routes", q, qPrintable(host));

			state = WaitingForResponse;
			respondError(502, "Bad Gateway", QString("No route for host: %1").arg(host));
			return;
		}

		log_debug("proxysession: %p %s has %d routes", q, qPrintable(host), route.targets.count());
	}

	void processIncomingRequest()
	{
		if(state == Prefetching)
		{
			in += zhttpRequest->readBody(MAX_PREFETCH_REQUEST_BODY - in.size());

			if(in.size() >= MAX_PREFETCH_REQUEST_BODY || zhttpRequest->isInputFinished())
			{
				// we've read enough body to start inspection

				disconnect(zhttpRequest, SIGNAL(readyRead()), this, SLOT(zhttpRequest_readyRead()));

				state = Inspecting;
				requestData.body = in.toByteArray();
				bool truncated = (!zhttpRequest->isInputFinished() || zhttpRequest->bytesAvailable() > 0);

				assert(!inspectRequest);

				if(inspectManager)
				{
					inspectRequest = new InspectRequest(inspectManager, this);

					if(inspectChecker->isInterfaceAvailable())
					{
						connect(inspectRequest, SIGNAL(finished()), SLOT(inspectRequest_finished()));
						inspectChecker->watch(inspectRequest);
						inspectRequest->start(requestData, truncated, route.session);
					}
					else
					{
						inspectChecker->watch(inspectRequest);
						inspectChecker->give(inspectRequest);
						inspectRequest->start(requestData, truncated, route.session);
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

				disconnect(zhttpRequest, SIGNAL(readyRead()), this, SLOT(zhttpRequest_readyRead()));

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
			QJson::Serializer serializer;

			QByteArray reasonJson = serializer.serialize(QString::fromUtf8(reason));
			if(reasonJson.isNull())
				return QByteArray();

			QVariantMap vheaders;
			foreach(const HttpHeader h, headers)
			{
				if(!vheaders.contains(h.first))
					vheaders[h.first] = h.second;
			}

			QByteArray headersJson = serializer.serialize(vheaders);
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
			QJson::Serializer serializer;

			QByteArray bodyJson = serializer.serialize(buf);
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

		QByteArray callbackParam = config.callbackParam;
		if(callbackParam.isEmpty())
			callbackParam = "callback";

		// two ways to activate JSON-P:
		//   1) callback param present
		//   2) default callback specified in configuration and body param present
		if(!requestData.uri.hasEncodedQueryItem(callbackParam) &&
			(config.defaultCallback.isEmpty() || config.bodyParam.isEmpty() || !requestData.uri.hasEncodedQueryItem(config.bodyParam)))
		{
			return false;
		}

		QUrl uri = requestData.uri;

		QByteArray callback;
		if(uri.hasEncodedQueryItem(callbackParam))
		{
			callback = parsePercentEncoding(uri.encodedQueryItemValue(callbackParam));
			if(callback.isEmpty())
			{
				log_warning("requestsession: id=%s invalid callback parameter, rejecting", rid.second.data());
				*ok = false;
				*errorMessage = "Invalid callback parameter.";
				return false;
			}

			uri.removeAllEncodedQueryItems(callbackParam);
		}
		else
			callback = config.defaultCallback;

		QByteArray method;
		if(uri.hasEncodedQueryItem("_method"))
		{
			method = parsePercentEncoding(uri.encodedQueryItemValue("_method"));
			if(!validMethod(method))
			{
				log_warning("requestsession: id=%s invalid _method parameter, rejecting", rid.second.data());
				*ok = false;
				*errorMessage = "Invalid _method parameter.";
				return false;
			}

			uri.removeAllEncodedQueryItems("_method");
		}

		HttpHeaders headers;
		if(uri.hasEncodedQueryItem("_headers"))
		{
			QJson::Parser parser;
			bool parserOk;
			QVariant vheaders = parser.parse(parsePercentEncoding(uri.encodedQueryItemValue("_headers")), &parserOk);
			if(!parserOk || vheaders.type() != QVariant::Map)
			{
				log_warning("requestsession: id=%s invalid _headers parameter, rejecting", rid.second.data());
				*ok = false;
				*errorMessage = "Invalid _headers parameter.";
				return false;
			}

			QVariantMap headersMap = vheaders.toMap();
			QMapIterator<QString, QVariant> vit(headersMap);
			while(vit.hasNext())
			{
				vit.next();

				if(vit.value().type() != QVariant::String)
				{
					log_warning("requestsession: id=%s invalid _headers parameter, rejecting", rid.second.data());
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

			uri.removeAllEncodedQueryItems("_headers");
		}

		QByteArray body;
		if(!config.bodyParam.isEmpty())
		{
			if(uri.hasEncodedQueryItem(config.bodyParam))
			{
				body = parsePercentEncoding(uri.encodedQueryItemValue(config.bodyParam));
				if(body.isNull())
				{
					log_warning("requestsession: id=%s invalid body parameter, rejecting", rid.second.data());
					*ok = false;
					*errorMessage = "Invalid body parameter.";
					return false;
				}

				headers.removeAll("Content-Type");
				headers += HttpHeader("Content-Type", "application/json");

				uri.removeAllEncodedQueryItems(config.bodyParam);
			}
		}
		else
		{
			if(uri.hasEncodedQueryItem("_body"))
			{
				body = parsePercentEncoding(uri.encodedQueryItemValue("_body"));
				if(body.isNull())
				{
					log_warning("requestsession: id=%s invalid _body parameter, rejecting", rid.second.data());
					*ok = false;
					*errorMessage = "Invalid _body parameter.";
					return false;
				}

				uri.removeAllEncodedQueryItems("_body");
			}
		}

		// if we have no query items anymore, strip the '?'
		if(uri.encodedQueryItems().isEmpty())
		{
			QByteArray tmp = uri.toEncoded();
			if(tmp.length() > 0 && tmp[tmp.length() - 1] == '?')
			{
				tmp.truncate(tmp.length() - 1);
				uri = QUrl(tmp, QUrl::StrictMode);
			}
		}

		if(method.isEmpty())
			method = "POST";

		requestData.method = method;

		requestData.uri = uri;

		headers += HttpHeader("Host", uri.host().toUtf8());
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
			areq.https = zhttpRequest->requestUri().scheme() == "https";
			areq.peerAddress = zhttpRequest->peerAddress();
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
			adata.inspectData.doProxy = idata.doProxy;
			adata.inspectData.sharingKey = idata.sharingKey;
			adata.inspectData.userData = idata.userData;

			adata.route = route.id;
			adata.channelPrefix = route.prefix;

			acceptRequest = new AcceptRequest(acceptManager, this);
			connect(acceptRequest, SIGNAL(finished()), SLOT(acceptRequest_finished()));
			acceptRequest->start(adata);
		}
		else
		{
			emit q->paused();
		}
	}

	void zhttpRequest_error()
	{
		log_warning("requestsession: request error id=%s", rid.second.data());
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
			connect(zhttpRequest, SIGNAL(readyRead()), SLOT(zhttpRequest_readyRead()));
			processIncomingRequest();
		}
		else
		{
			if(!idata.sharingKey.isEmpty())
			{
				// a request can only be shared if we've read the entire
				//   request body, so let's try to read it now
				state = Receiving;

				connect(zhttpRequest, SIGNAL(readyRead()), SLOT(zhttpRequest_readyRead()));
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
				// the request was paused, so deleting it will leave the peer session active
				delete zhttpRequest;
				zhttpRequest = 0;

				state = Stopped;

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

					connect(zhttpRequest, SIGNAL(bytesWritten(int)), SLOT(zhttpRequest_bytesWritten(int)));

					zhttpRequest->beginResponse(200, "OK", headers);

					jsonpTracker.addPlain(bodyRawBuf.size());
					jsonpTracker.specifyEncoded(buf.size(), bodyRawBuf.size());

					zhttpRequest->writeBody(buf);
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
					zhttpRequest->endBody();
					emit q->errorResponding();
					return;
				}

				headers += HttpHeader("Content-Type", "application/javascript");
				headers += HttpHeader("Transfer-Encoding", "chunked");

				connect(zhttpRequest, SIGNAL(bytesWritten(int)), SLOT(zhttpRequest_bytesWritten(int)));

				zhttpRequest->beginResponse(200, "OK", headers);

				jsonpTracker.specifyEncoded(buf.size(), 0);

				zhttpRequest->writeBody(buf);
			}
			else
			{
				if(autoCrossOrigin || (!route.isNull() && route.autoCrossOrigin))
					applyCorsHeaders(requestData.headers, &responseData.headers);

				connect(zhttpRequest, SIGNAL(bytesWritten(int)), SLOT(zhttpRequest_bytesWritten(int)));

				zhttpRequest->beginResponse(responseData.code, responseData.reason, responseData.headers);
			}
		}

		if(!out.isEmpty())
		{
			if(!jsonpCallback.isEmpty())
			{
				QByteArray bodyRawBuf = out.take();
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
			}
			else
			{
				zhttpRequest->writeBody(out.take());
			}
		}

		if(responseBodyFinished)
		{
			if(!jsonpCallback.isEmpty())
			{
				QByteArray buf = makeJsonpEnd();
				jsonpTracker.specifyEncoded(buf.size(), 0);
				zhttpRequest->writeBody(buf);
			}

			zhttpRequest->endBody();
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

RequestSession::RequestSession(DomainMap *domainMap, SockJsManager *sockJsManager, ZrpcManager *inspectManager, ZrpcChecker *inspectChecker, ZrpcManager *acceptManager, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, domainMap, sockJsManager, inspectManager, inspectChecker, acceptManager);
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
	return d->zhttpRequest->requestUri().scheme() == "https";
}

QHostAddress RequestSession::peerAddress() const
{
	return d->zhttpRequest->peerAddress();
}

ZhttpRequest::Rid RequestSession::rid() const
{
	return d->rid;
}

HttpRequestData RequestSession::requestData() const
{
	return d->requestData;
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

void RequestSession::setAutoCrossOrigin(bool enabled)
{
	d->autoCrossOrigin = enabled;
}

void RequestSession::start(ZhttpRequest *req)
{
	d->start(req);
}

void RequestSession::startRetry(ZhttpRequest *req, bool autoCrossOrigin, const QByteArray &jsonpCallback, bool jsonpExtendedResponse)
{
	d->isRetry = true;
	d->zhttpRequest = req;
	d->rid = req->rid();
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
	assert(d->state == Private::WaitingForResponse);

	d->zhttpRequest->pause();
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
