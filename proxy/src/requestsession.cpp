/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
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
#include "log.h"
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
	RequestSession *q;
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

	Private(RequestSession *_q, InspectManager *_inspectManager, InspectChecker *_inspectChecker) :
		QObject(_q),
		q(_q),
		inspectManager(_inspectManager),
		inspectChecker(_inspectChecker),
		m2Request(0),
		m2Response(0),
		m2Manager(0),
		isHttps(false),
		inspectRequest(0)
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

			emit q->finishedForAccept(adata);
		}
	}

	void respondError(int code, const QString &status, const QString &errorString)
	{
		m2Response = q->createResponse();
		connect(m2Response, SIGNAL(finished()), SLOT(m2Response_finished()));

		QByteArray body = errorString.toUtf8() + '\n';

		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		headers += HttpHeader("Content-Length", QByteArray::number(body.size()));

		// in case we were reading a request in progress, delete here to stop it
		delete m2Request;
		m2Request = 0;

		if(!jsonpCallback.isEmpty())
		{
			QJson::Serializer serializer;

			QVariantMap vheaders;
			foreach(const HttpHeader h, headers)
			{
				if(!vheaders.contains(h.first))
					vheaders[h.first] = h.second;
			}

			QVariantMap vresult;
			vresult["code"] = code;
			vresult["status"] = status.toUtf8();
			vresult["headers"] = vheaders;
			vresult["body"] = body;

			QByteArray resultJson = serializer.serialize(vresult);
			assert(!resultJson.isNull());

			QByteArray body = jsonpCallback + '(' + resultJson + ");\n";

			HttpHeaders jheaders;
			jheaders.removeAll("Content-Length");
			jheaders += HttpHeader("Content-Type", "application/javascript");
			jheaders += HttpHeader("Content-Length", QByteArray::number(body.size()));

			m2Response->start(200, "OK", jheaders);
			m2Response->write(body);
			m2Response->close();
		}
		else
		{
			m2Response->start(code, status.toLatin1(), headers);
			m2Response->write(body);
			m2Response->close();
		}
	}

	void respondBadRequest(const QString &errorString)
	{
		respondError(400, "Bad Request", errorString);
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
		log_error("requestsession: request error: %d", m2Request->rid().second.data());
		cleanup();
		emit q->finished();
	}

	void m2Response_finished()
	{
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
			// successful inspect indicated we should not proxy. in that case,
			//   collect the body and accept
			connect(m2Request, SIGNAL(readyRead()), SLOT(m2Request_readyRead()));
			connect(m2Request, SIGNAL(finished()), SLOT(m2Request_finished()));
			processIncomingRequest();
		}
		else
			emit q->inspected(idata);
	}

	void inspectRequest_error()
	{
		delete inspectRequest;
		inspectRequest = 0;

		emit q->inspectError();
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

M2Response *RequestSession::createResponse()
{
	if(d->m2Request)
		return d->m2Request->createResponse();
	else
		return d->m2Manager->createResponse(d->rid);
}

void RequestSession::cannotAccept()
{
	d->respondError(500, "Internal Server Error", "Accept service unavailable");
}

#include "requestsession.moc"
