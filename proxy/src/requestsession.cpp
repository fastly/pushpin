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

#include "requestsession.h"

#include <assert.h>
#include <QUrl>
#include <qjson/parser.h>
#include <qjson/serializer.h>
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "log.h"
#include "layertracker.h"
#include "inspectdata.h"
#include "acceptdata.h"
#include "m2request.h"
#include "m2response.h"
#include "m2manager.h"
#include "inspectmanager.h"
#include "inspectrequest.h"
#include "inspectchecker.h"

#define MAX_ACCEPT_REQUEST_BODY 100000

static bool parseHostHeader(bool https, const QByteArray &in, QString *_host, int *_port)
{
	QString host = QString::fromUtf8(in);

	int port;
	int at = host.lastIndexOf(':');
	if(at != -1)
	{
		QString sport = host.mid(at + 1);
		bool ok;
		port = sport.toInt(&ok);
		if(!ok)
			return false;

		host = host.mid(0, at);
	}
	else
	{
		if(https)
			port = 443;
		else
			port = 80;
	}

	if(_host)
		*_host = host;
	if(_port)
		*_port = port;

	return true;
}

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
		Responding,
		RespondingInternal
	};

	RequestSession *q;
	State state;
	InspectManager *inspectManager;
	InspectChecker *inspectChecker;
	M2Request *m2Request;
	M2Response *m2Response;
	M2Manager *m2Manager; // used when we don't have m2Request
	M2Request::Rid rid;
	bool isHttps;
	HttpRequestData requestData;
	InspectRequest *inspectRequest;
	InspectData idata;
	QString host;
	QByteArray in;
	QByteArray jsonpCallback;
	HttpResponseData responseData;
	bool responseBodyFinished;
	bool pendingResponseUpdate;
	LayerTracker jsonpTracker;

	Private(RequestSession *_q, InspectManager *_inspectManager, InspectChecker *_inspectChecker) :
		QObject(_q),
		q(_q),
		state(Stopped),
		inspectManager(_inspectManager),
		inspectChecker(_inspectChecker),
		m2Request(0),
		m2Response(0),
		m2Manager(0),
		isHttps(false),
		inspectRequest(0),
		responseBodyFinished(false),
		pendingResponseUpdate(false)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(m2Request)
		{
			delete m2Request;
			m2Request = 0;
		}

		if(m2Response)
		{
			delete m2Response;
			m2Response = 0;
		}

		if(inspectRequest)
		{
			inspectChecker->disconnect(this);
			inspectChecker->give(inspectRequest);
			inspectChecker = 0;
		}

		state = Stopped;
	}

	void start(M2Request *req)
	{
		m2Request = req;

		QByteArray rawHost = req->headers().get("host");
		if(rawHost.isEmpty())
		{
			log_warning("requestsession: no host header, rejecting");
			respondBadRequest("Host header required.");
			return;
		}

		int port;
		if(!parseHostHeader(req->isHttps(), rawHost, &host, &port))
		{
			log_warning("requestsession: invalid host header, rejecting");
			respondBadRequest("Invalid host header.");
			return;
		}

		QByteArray scheme;
		if(req->isHttps())
			scheme = "https";
		else
			scheme = "http";

		QByteArray urlstr = scheme + "://" + host.toUtf8();
		if((req->isHttps() && port != 443) || (!req->isHttps() && port != 80))
			urlstr += ':' + QByteArray::number(port);
		urlstr += req->path();

		log_info("IN id=%d, %s %s", req->rid().second.data(), qPrintable(req->method()), qPrintable(urlstr));

		connect(m2Request, SIGNAL(error()), SLOT(m2Request_error()));

		QUrl url(urlstr);
		HttpRequestData hdata;

		// JSON-P
		if(url.hasQueryItem("_callback"))
		{
			bool callbackDone = false;
			bool methodDone = false;
			bool headersDone = false;
			bool bodyDone = false;

			QList< QPair<QByteArray, QByteArray> > encodedItems = url.encodedQueryItems();
			for(int n = 0; n < encodedItems.count(); ++n)
			{
				const QPair<QByteArray, QByteArray> &i = encodedItems[n];

				QByteArray name = parsePercentEncoding(i.first);
				if(name == "_callback")
				{
					if(callbackDone)
						continue;

					callbackDone = true;

					QByteArray callback = parsePercentEncoding(i.second);
					if(callback.isEmpty())
					{
						log_warning("requestsession: invalid _callback parameter, rejecting");
						respondBadRequest("Invalid _callback parameter.");
						return;
					}

					jsonpCallback = callback;
					url.removeAllQueryItems("_callback");
				}
				else if(name == "_method")
				{
					if(methodDone)
						continue;

					methodDone = true;
					QByteArray method = parsePercentEncoding(i.second);

					if(!validMethod(method))
					{
						log_warning("requestsession: invalid _method parameter, rejecting");
						respondBadRequest("Invalid _method parameter.");
						return;
					}

					hdata.method = method;
					url.removeAllQueryItems("_method");
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
						log_warning("requestsession: invalid _headers parameter, rejecting");
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
							log_warning("requestsession: invalid _headers parameter, rejecting");
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
					url.removeAllQueryItems("_headers");
				}
				else if(name == "_body")
				{
					if(bodyDone)
						continue;

					bodyDone = true;
					hdata.body = parsePercentEncoding(i.second);
					url.removeAllQueryItems("_body");
				}
			}

			if(hdata.method.isEmpty())
				hdata.method = "GET";

			hdata.path = url.encodedPath();
			if(url.hasQuery())
				hdata.path += "?" + url.encodedQuery();

			hdata.headers += HttpHeader("Host", host.toUtf8());
			hdata.headers += HttpHeader("Accept", "*/*");

			// carry over the rest of the headers
			foreach(const HttpHeader &h, req->headers())
			{
				if(!hdata.headers.contains(h.first))
					hdata.headers += h;
			}
		}
		else
		{
			hdata.method = req->method();
			hdata.path = req->path();
			hdata.headers = req->headers();
		}

		rid = m2Request->rid();
		isHttps = m2Request->isHttps();
		requestData = hdata;

		// NOTE: per the license, this functionality may not be removed as it
		//   is the interface for the copyright notice
		if(requestData.headers.contains("Pushpin-Check"))
		{
			QString str =
			"Copyright (C) 2012-2013 Fan Out Networks, Inc.\n"
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

	void processIncomingRequest()
	{
		QByteArray buf = m2Request->read();
		if(in.size() + buf.size() > MAX_ACCEPT_REQUEST_BODY)
		{
			respondError(413, "Request Entity Too Large", QString("Body must not exceed %1 bytes").arg(MAX_ACCEPT_REQUEST_BODY));
			return;
		}

		in += buf;

		if(m2Request->isFinished())
		{
			AcceptData adata;

			AcceptData::Request areq;
			areq.rid = m2Request->rid();
			areq.https = isHttps;
			areq.jsonpCallback = jsonpCallback;
			adata.requests += areq;

			adata.requestData.method = m2Request->method();
			adata.requestData.path = m2Request->path();
			adata.requestData.headers = m2Request->headers();

			adata.haveInspectData = true;
			adata.inspectData.doProxy = idata.doProxy;
			adata.inspectData.sharingKey = idata.sharingKey;
			adata.inspectData.userData = idata.userData;

			m2Manager = m2Request->managerForResponse();

			delete m2Request;
			m2Request = 0;

			state = Stopped;

			emit q->finishedForAccept(adata);
		}
	}

	void respond(int code, const QString &status, const QByteArray &body)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		headers += HttpHeader("Content-Length", QByteArray::number(body.size()));

		q->startResponse(code, status.toUtf8(), headers);

		// in case we were reading a request in progress, delete here to stop it
		delete m2Request;
		m2Request = 0;

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
	QByteArray makeJsonpStart(int code, const QByteArray &status, const HttpHeaders &headers)
	{
		QJson::Serializer serializer;

		QByteArray statusJson = serializer.serialize(QString::fromUtf8(status));
		if(statusJson.isNull())
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

		return jsonpCallback + "({\"code\": " + QByteArray::number(code) + ", \"status\": " + statusJson + ", \"headers\": " + headersJson + ", \"body\": \"";
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
	void m2Request_readyRead()
	{
		processIncomingRequest();
	}

	void m2Request_finished()
	{
		processIncomingRequest();
	}

	void m2Request_error()
	{
		log_warning("requestsession: request error: %d", m2Request->rid().second.data());
		cleanup();
		emit q->finished();
	}

	void m2Response_bytesWritten(int count)
	{
		if(!jsonpCallback.isEmpty())
		{
			int actual = jsonpTracker.finished(count);
			if(actual > 0)
				emit q->bytesWritten(actual);
		}
		else
			emit q->bytesWritten(count);
	}

	void m2Response_finished()
	{
		cleanup();
		emit q->finished();
	}

	void m2Response_error()
	{
		log_warning("requestsession: response error: %d", m2Response->rid().second.data());
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
			connect(m2Request, SIGNAL(readyRead()), SLOT(m2Request_readyRead()));
			connect(m2Request, SIGNAL(finished()), SLOT(m2Request_finished()));
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
		log_debug("update");
		pendingResponseUpdate = false;

		if(!m2Response)
		{
			if(m2Request)
				m2Response = m2Request->createResponse();
			else
				m2Response = m2Manager->createResponse(rid);

			connect(m2Response, SIGNAL(finished()), SLOT(m2Response_finished()));
			connect(m2Response, SIGNAL(error()), SLOT(m2Response_error()));

			if(!jsonpCallback.isEmpty())
			{
				HttpHeaders headers;

				if(responseBodyFinished)
				{
					QByteArray startBuf = makeJsonpStart(responseData.code, responseData.status, responseData.headers);
					QByteArray bodyBuf;
					QByteArray endBuf = makeJsonpEnd();
					if(!startBuf.isNull())
						bodyBuf = makeJsonpBody(responseData.body);

					if(startBuf.isNull() || bodyBuf.isNull())
					{
						state = RespondingInternal;

						QByteArray body = "Upstream response could not be JSON-P encoded.\n";
						headers += HttpHeader("Content-Type", "text/plain");
						headers += HttpHeader("Content-Length", QByteArray::number(body.size()));
						m2Response->start(500, "Internal Server Error", headers);
						m2Response->write(body);
						m2Response->close();
						emit q->errorResponding();
						return;
					}

					QByteArray buf = startBuf + bodyBuf + endBuf;

					headers += HttpHeader("Content-Type", "application/javascript");
					headers += HttpHeader("Content-Length", QByteArray::number(buf.size()));

					connect(m2Response, SIGNAL(bytesWritten(int)), SLOT(m2Response_bytesWritten(int)));

					m2Response->start(200, "OK", headers);

					jsonpTracker.addPlain(responseData.body.size());
					jsonpTracker.specifyEncoded(buf.size(), responseData.body.size());

					m2Response->write(buf);

					responseData.body.clear();

					m2Response->close();
					return;
				}

				QByteArray buf = makeJsonpStart(responseData.code, responseData.status, responseData.headers);
				if(buf.isNull())
				{
					state = RespondingInternal;

					QByteArray body = "Upstream response could not be JSON-P encoded.\n";
					headers += HttpHeader("Content-Type", "text/plain");
					headers += HttpHeader("Content-Length", QByteArray::number(body.size()));
					m2Response->start(500, "Internal Server Error", headers);
					m2Response->write(body);
					m2Response->close();
					emit q->errorResponding();
					return;
				}

				headers += HttpHeader("Content-Type", "application/javascript");
				headers += HttpHeader("Transfer-Encoding", "chunked");

				connect(m2Response, SIGNAL(bytesWritten(int)), SLOT(m2Response_bytesWritten(int)));

				m2Response->start(200, "OK", headers);

				jsonpTracker.specifyEncoded(buf.size(), 0);

				m2Response->write(buf);
			}
			else
			{
				connect(m2Response, SIGNAL(bytesWritten(int)), SLOT(m2Response_bytesWritten(int)));

				m2Response->start(responseData.code, responseData.status, responseData.headers);
			}
		}

		if(!responseData.body.isEmpty())
		{
			if(!jsonpCallback.isEmpty())
			{
				QByteArray buf = makeJsonpBody(responseData.body);
				if(buf.isNull())
				{
					state = RespondingInternal;

					log_warning("upstream response could not be JSON-P encoded");

					// if we error while streaming, all we can do is give up
					m2Response->close();
					emit q->errorResponding();
					return;
				}

				jsonpTracker.addPlain(responseData.body.size());
				jsonpTracker.specifyEncoded(buf.size(), responseData.body.size());

				m2Response->write(buf);
			}
			else
			{
				m2Response->write(responseData.body);
			}

			responseData.body.clear();
		}

		if(responseBodyFinished)
		{
			if(!jsonpCallback.isEmpty())
			{
				QByteArray buf = makeJsonpEnd();
				jsonpTracker.specifyEncoded(buf.size(), 0);
				m2Response->write(buf);
			}

			m2Response->close();
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
	return d->m2Request ? false : true;
}

bool RequestSession::isHttps() const
{
	return d->isHttps;
}

QString RequestSession::host() const
{
	return d->host;
}

M2Request::Rid RequestSession::rid() const
{
	return d->rid;
}

HttpRequestData RequestSession::requestData() const
{
	return d->requestData;
}

QByteArray RequestSession::jsonpCallback() const
{
	return d->jsonpCallback;
}

M2Request *RequestSession::request()
{
	return d->m2Request;
}

void RequestSession::start(M2Request *req)
{
	d->start(req);
}

bool RequestSession::setupAsRetry(const M2Request::Rid &rid, const HttpRequestData &hdata, bool https, const QByteArray &jsonpCallback, M2Manager *manager)
{
	d->rid = rid;
	d->requestData = hdata;
	d->isHttps = https;
	d->jsonpCallback = jsonpCallback;
	d->m2Manager = manager;

	if(!parseHostHeader(d->isHttps, d->requestData.headers.get("host"), &d->host, 0))
		return false;

	return true;
}

void RequestSession::startResponse(int code, const QByteArray &status, const HttpHeaders &headers)
{
	assert(d->state == Private::Accepting || d->state == Private::WaitingForResponse);

	d->state = Private::Responding;

	d->responseData.code = code;
	d->responseData.status = status;
	d->responseData.headers = headers;

	d->responseUpdate();
}

void RequestSession::writeResponseBody(const QByteArray &body)
{
	assert(d->state == Private::Responding);
	assert(!d->responseBodyFinished);

	d->responseData.body += body;
	d->responseUpdate();
}

void RequestSession::endResponseBody()
{
	assert(d->state == Private::Responding);
	assert(!d->responseBodyFinished);

	d->responseBodyFinished = true;
	d->responseUpdate();
}

void RequestSession::respondError(int code, const QString &status, const QString &errorString)
{
	d->respondError(code, status, errorString);
}

void RequestSession::respondCannotAccept()
{
	respondError(500, "Internal Server Error", "Accept service unavailable.");
}

#include "requestsession.moc"
