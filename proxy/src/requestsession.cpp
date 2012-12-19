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

class RequestSession::Private : public QObject
{
	Q_OBJECT

public:
	RequestSession *q;
	InspectManager *inspectManager;
	InspectChecker *inspectChecker;
	M2Request *m2Request;
	M2Response *m2Response;
	M2Manager *m2Manager; // only used in retry mode, otherwise we get from m2Request
	M2Request::Rid retryRid;
	HttpRequestData *retryData;
	bool retryIsHttps;
	InspectRequest *inspectRequest;
	InspectData idata;
	QString host;
	QByteArray in;

	Private(RequestSession *_q, InspectManager *_inspectManager, InspectChecker *_inspectChecker) :
		QObject(_q),
		q(_q),
		inspectManager(_inspectManager),
		inspectChecker(_inspectChecker),
		m2Request(0),
		m2Response(0),
		m2Manager(0),
		retryData(0),
		retryIsHttps(false),
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

		QByteArray url = scheme + "://" + host.toUtf8();
		if((req->isHttps() && port != 443) || (!req->isHttps() && port != 80))
			url += ':' + QByteArray::number(port);
		url += req->path();

		log_info("IN id=%d, %s %s", req->rid().second.data(), qPrintable(req->method()), qPrintable(url));

		connect(m2Request, SIGNAL(error()), SLOT(m2Request_error()));

		inspectRequest = inspectManager->createRequest();

		HttpRequestData hdata;
		hdata.method = req->method();
		hdata.path = req->path();
		hdata.headers = req->headers();

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
			adata.rids += m2Request->rid();

			adata.request.method = m2Request->method();
			adata.request.path = m2Request->path();
			adata.request.headers = m2Request->headers();

			adata.https = m2Request->isHttps();

			adata.haveInspectData = true;
			adata.inspectData.doProxy = idata.doProxy;
			adata.inspectData.sharingKey = idata.sharingKey;
			adata.inspectData.userData = idata.userData;

			delete m2Request;
			m2Request = 0;

			emit q->finishedForAccept(adata);
		}
	}

	void respondError(int code, const QString &status, const QString &errorString)
	{
		m2Response = m2Request->createResponse();
		connect(m2Response, SIGNAL(finished()), SLOT(m2Response_finished()));

		QByteArray body = errorString.toUtf8() + '\n';

		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		headers += HttpHeader("Content-Length", QByteArray::number(body.length()));

		// in case we were reading a request in progress, delete here to stop it
		delete m2Request;
		m2Request = 0;

		m2Response->start(code, status.toLatin1(), headers);
		m2Response->write(body);
		m2Response->close();
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
	return d->retryData;
}

bool RequestSession::isHttps() const
{
	if(d->m2Request)
		return d->m2Request->isHttps();
	else
		return d->retryIsHttps;
}

QString RequestSession::host() const
{
	return d->host;
}

M2Request *RequestSession::request()
{
	return d->m2Request;
}

M2Request::Rid RequestSession::retryRid()
{
	return d->retryRid;
}

HttpRequestData RequestSession::retryData()
{
	return *d->retryData;
}

void RequestSession::start(M2Request *req)
{
	d->start(req);
}

bool RequestSession::setupAsRetry(const M2Request::Rid &rid, const HttpRequestData &hdata, bool https, M2Manager *manager)
{
	d->retryRid = rid;
	d->retryData = new HttpRequestData(hdata);
	d->retryIsHttps = https;
	d->m2Manager = manager;

	if(!parseHostHeader(https, d->retryData->headers.get("host"), &d->host, 0))
		return false;

	return true;
}

M2Response *RequestSession::createResponse()
{
	if(d->m2Request)
		return d->m2Request->createResponse();
	else
		return d->m2Manager->createResponse(d->retryRid);
}

#include "requestsession.moc"
