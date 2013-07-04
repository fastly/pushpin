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
#include "inspectdata.h"
#include "acceptdata.h"
#include "inspectmanager.h"
#include "inspectrequest.h"
#include "inspectchecker.h"

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
				break;

			int hi = fromHex(in[n + 1]);
			if(hi == -1)
				break;

			int lo = fromHex(in[n + 2]);
			if(lo == -1)
				break;

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

class RequestSession::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Stopped,
		Inspecting,
		Accepting,
		WaitingForResponse,
		RespondingStart,
		Responding,
		RespondingInternal
	};

	RequestSession *q;
	State state;
	ZhttpRequest::Rid rid;
	InspectManager *inspectManager;
	InspectChecker *inspectChecker;
	ZhttpRequest *zhttpRequest;
	HttpRequestData requestData;
	bool autoCrossOrigin;
	InspectRequest *inspectRequest;
	InspectData idata;
	BufferList in;
	QByteArray jsonpCallback;
	HttpResponseData responseData;
	BufferList out;
	bool responseBodyFinished;
	bool pendingResponseUpdate;
	LayerTracker jsonpTracker;
	bool isRetry;

	Private(RequestSession *_q, InspectManager *_inspectManager, InspectChecker *_inspectChecker) :
		QObject(_q),
		q(_q),
		state(Stopped),
		inspectManager(_inspectManager),
		inspectChecker(_inspectChecker),
		zhttpRequest(0),
		autoCrossOrigin(false),
		inspectRequest(0),
		responseBodyFinished(false),
		pendingResponseUpdate(false),
		isRetry(false)
	{
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
			inspectChecker->disconnect(this);
			inspectChecker->give(inspectRequest);
			inspectChecker = 0;
		}

		state = Stopped;
	}

	void start(ZhttpRequest *req)
	{
		zhttpRequest = req;
		rid = req->rid();

		QUrl uri = req->requestUri();

		log_info("IN id=%s, %s %s", rid.second.data(), qPrintable(req->requestMethod()), uri.toEncoded().data());

		connect(zhttpRequest, SIGNAL(error()), SLOT(zhttpRequest_error()));
		connect(zhttpRequest, SIGNAL(paused()), SLOT(zhttpRequest_paused()));

		HttpRequestData hdata;

		// JSON-P
		if(autoCrossOrigin && uri.hasQueryItem("callback"))
		{
			bool callbackDone = false;
			bool methodDone = false;
			bool headersDone = false;
			bool bodyDone = false;

			QList< QPair<QByteArray, QByteArray> > encodedItems = uri.encodedQueryItems();
			for(int n = 0; n < encodedItems.count(); ++n)
			{
				const QPair<QByteArray, QByteArray> &i = encodedItems[n];

				QByteArray name = parsePercentEncoding(i.first);
				if(name == "callback")
				{
					if(callbackDone)
						continue;

					callbackDone = true;

					QByteArray callback = parsePercentEncoding(i.second);
					if(callback.isEmpty())
					{
						log_warning("requestsession: id=%s invalid callback parameter, rejecting", rid.second.data());
						respondBadRequest("Invalid callback parameter.");
						return;
					}

					jsonpCallback = callback;
					uri.removeAllQueryItems("callback");
				}
				else if(name == "_method")
				{
					if(methodDone)
						continue;

					methodDone = true;
					QByteArray method = parsePercentEncoding(i.second);

					if(!validMethod(method))
					{
						log_warning("requestsession: id=%s invalid _method parameter, rejecting", rid.second.data());
						respondBadRequest("Invalid _method parameter.");
						return;
					}

					hdata.method = method;
					uri.removeAllQueryItems("_method");
				}
				else if(name == "_headers")
				{
					if(headersDone)
						continue;

					headersDone = true;

					QJson::Parser parser;
					bool ok;
					QVariant vheaders = parser.parse(parsePercentEncoding("_headers"), &ok);
					if(!ok)
					{
						log_warning("requestsession: id=%s invalid _headers parameter, rejecting", rid.second.data());
						respondBadRequest("Invalid _headers parameter.");
						return;
					}

					QVariantMap headersMap = vheaders.toMap();
					HttpHeaders headers;
					QMapIterator<QString, QVariant> vit(headersMap);
					while(vit.hasNext())
					{
						vit.next();

						if(vit.value().type() != QVariant::String)
						{
							log_warning("requestsession: id=%s invalid _headers parameter, rejecting", rid.second.data());
							respondBadRequest("Invalid _headers parameter.");
							return;
						}

						QByteArray key = vit.key().toUtf8();

						// ignore some headers that we explicitly set later on
						if(qstricmp(key.data(), "host") == 0)
							continue;
						if(qstricmp(key.data(), "accept") == 0)
							continue;

						headers += HttpHeader(key, vit.value().toString().toUtf8());
					}

					hdata.headers = headers;
					uri.removeAllQueryItems("_headers");
				}
				else if(name == "_body")
				{
					if(bodyDone)
						continue;

					bodyDone = true;
					hdata.body = parsePercentEncoding(i.second);
					uri.removeAllQueryItems("_body");
				}
			}

			if(hdata.method.isEmpty())
				hdata.method = "GET";

			hdata.uri = uri;

			hdata.headers += HttpHeader("Host", uri.host().toUtf8());
			hdata.headers += HttpHeader("Accept", "*/*");

			// carry over the rest of the headers
			foreach(const HttpHeader &h, req->requestHeaders())
			{
				if(qstricmp(h.first.data(), "host") == 0)
					continue;
				if(qstricmp(h.first.data(), "accept") == 0)
					continue;

				hdata.headers += h;
			}
		}
		else
		{
			hdata.method = req->requestMethod();
			hdata.uri = uri;
			hdata.headers = req->requestHeaders();
		}

		requestData = hdata;

		// NOTE: per the license, this functionality may not be removed as it
		//   is the interface for the copyright notice
		if(requestData.headers.contains("Pushpin-Check"))
		{
			QString str =
			"Copyright (C) 2012-2013 Fanout, Inc.\n"
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

			QMetaObject::invokeMethod(this, "respondSuccess", Qt::QueuedConnection, Q_ARG(QString, str));
			return;
		}

		state = Inspecting;

		inspectRequest = inspectManager->createRequest();

		if(inspectChecker->isInterfaceAvailable())
		{
			connect(inspectRequest, SIGNAL(finished(const InspectData &)), SLOT(inspectRequest_finished(const InspectData &)));
			connect(inspectRequest, SIGNAL(error()), SLOT(inspectRequest_error()));
			inspectChecker->watch(inspectRequest);
			inspectRequest->start(hdata);
		}
		else
		{
			inspectChecker->watch(inspectRequest);
			inspectChecker->give(inspectRequest);
			inspectRequest->start(hdata);
			inspectRequest = 0;
			QMetaObject::invokeMethod(this, "inspectRequest_error", Qt::QueuedConnection);
		}
	}

	void startRetry()
	{
		connect(zhttpRequest, SIGNAL(error()), SLOT(zhttpRequest_error()));
		connect(zhttpRequest, SIGNAL(paused()), SLOT(zhttpRequest_paused()));
	}

	void processIncomingRequest()
	{
		QByteArray buf = zhttpRequest->readBody();
		if(in.size() + buf.size() > MAX_ACCEPT_REQUEST_BODY)
		{
			respondError(413, "Request Entity Too Large", QString("Body must not exceed %1 bytes").arg(MAX_ACCEPT_REQUEST_BODY));
			return;
		}

		in += buf;

		if(zhttpRequest->isInputFinished())
			zhttpRequest->pause();
	}

	void respond(int code, const QString &status, const QByteArray &body)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		headers += HttpHeader("Content-Length", QByteArray::number(body.size()));

		q->startResponse(code, status.toUtf8(), headers);
		q->writeResponseBody(body);
		q->endResponseBody();
	}

	void respondError(int code, const QString &status, const QString &errorString)
	{
		respond(code, status, errorString.toUtf8() + '\n');
	}

	void respondBadRequest(const QString &errorString)
	{
		respondError(400, "Bad Request", errorString);
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

		return jsonpCallback + "({\"code\": " + QByteArray::number(code) + ", \"reason\": " + reasonJson + ", \"headers\": " + headersJson + ", \"body\": \"";
	}

	QByteArray makeJsonpBody(const QByteArray &buf)
	{
		QJson::Serializer serializer;

		QByteArray bodyJson = serializer.serialize(buf);
		if(bodyJson.isNull())
			return QByteArray();

		assert(bodyJson.size() >= 2);

		return bodyJson.mid(1, bodyJson.size() - 2);
	}

	QByteArray makeJsonpEnd()
	{
		return QByteArray("\"});\n");
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
		if(state == Accepting)
		{
			ZhttpRequest::ServerState ss = zhttpRequest->serverState();

			AcceptData adata;

			AcceptData::Request areq;
			areq.rid = rid;
			areq.https = zhttpRequest->requestUri().scheme() == "https";
			areq.peerAddress = zhttpRequest->peerAddress();
			areq.autoCrossOrigin = autoCrossOrigin;
			areq.jsonpCallback = jsonpCallback;
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

			// the request was paused, so deleting it will leave the peer session active
			delete zhttpRequest;
			zhttpRequest = 0;

			state = Stopped;

			emit q->finishedForAccept(adata);
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

	void inspectRequest_finished(const InspectData &_idata)
	{
		idata = _idata;

		delete inspectRequest;
		inspectRequest = 0;

		if(!idata.doProxy)
		{
			state = Accepting;

			// successful inspect indicated we should not proxy. in that case,
			//   collect the body and accept
			connect(zhttpRequest, SIGNAL(readyRead()), SLOT(zhttpRequest_readyRead()));
			processIncomingRequest();
		}
		else
		{
			state = WaitingForResponse;
			emit q->inspected(idata);
		}
	}

	void inspectRequest_error()
	{
		delete inspectRequest;
		inspectRequest = 0;

		state = WaitingForResponse;
		emit q->inspectError();
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
				if(autoCrossOrigin)
				{
					if(!responseData.headers.contains("Access-Control-Allow-Origin"))
					{
						QByteArray origin = requestData.headers.get("Origin");

						if(origin.isEmpty())
							origin = "*";

						responseData.headers += HttpHeader("Access-Control-Allow-Origin", origin);
						responseData.headers += HttpHeader("Access-Control-Allow-Methods", "OPTIONS, HEAD, GET, POST, PUT, DELETE");
					}
				}

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
};

RequestSession::RequestSession(InspectManager *inspectManager, InspectChecker *inspectChecker, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, inspectManager, inspectChecker);
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

void RequestSession::startRetry(ZhttpRequest *req, bool autoCrossOrigin, const QByteArray &jsonpCallback)
{
	d->isRetry = true;
	d->zhttpRequest = req;
	d->rid = req->rid();
	d->autoCrossOrigin = autoCrossOrigin;
	d->jsonpCallback = jsonpCallback;
	d->requestData.method = req->requestMethod();
	d->requestData.uri = req->requestUri();
	d->requestData.headers = req->requestHeaders();

	d->startRetry();
}

void RequestSession::pause()
{
	assert(d->state == Private::WaitingForResponse);

	d->zhttpRequest->pause();
}

void RequestSession::startResponse(int code, const QByteArray &reason, const HttpHeaders &headers)
{
	assert(d->state == Private::Accepting || d->state == Private::WaitingForResponse);

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

void RequestSession::respondError(int code, const QString &reason, const QString &errorString)
{
	d->respondError(code, reason, errorString);
}

void RequestSession::respondCannotAccept()
{
	respondError(500, "Internal Server Error", "Accept service unavailable.");
}

#include "requestsession.moc"
