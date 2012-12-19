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

#include "proxysession.h"

#include <assert.h>
#include <QSet>
#include <QUrl>
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "log.h"
#include "inspectdata.h"
#include "acceptdata.h"
#include "m2request.h"
#include "m2response.h"
#include "zurlmanager.h"
#include "zurlrequest.h"
#include "domainmap.h"
#include "requestsession.h"

#define MAX_ACCEPT_REQUEST_BODY 100000
#define MAX_ACCEPT_RESPONSE_BODY 100000

class ProxySession::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Stopped,
		Requesting,
		Accepting,
		Responding
	};

	ProxySession *q;
	State state;
	ZurlManager *zurlManager;
	DomainMap *domainMap;
	M2Request *m2Request;
	QString host;
	bool isHttps;
	ZurlRequest *zurlRequest;
	bool addAllowed;
	bool haveInspectData;
	InspectData idata;
	AcceptData adata;
	QSet<QByteArray> acceptTypes;
	QSet<RequestSession*> requestSessions;
	HttpRequestData requestData;
	HttpResponseData responseData;
	QSet<M2Response*> m2Responses;
	QHash<M2Response*, RequestSession*> sessionsByResponse;
	int total;

	Private(ProxySession *_q, ZurlManager *_zurlManager, DomainMap *_domainMap) :
		QObject(_q),
		q(_q),
		state(Stopped),
		zurlManager(_zurlManager),
		domainMap(_domainMap),
		m2Request(0),
		isHttps(false),
		zurlRequest(0),
		addAllowed(true),
		haveInspectData(false)
	{
		total = 0;
		acceptTypes += "application/x-fo-instruct";
		acceptTypes += "application/fo-instruct";
		acceptTypes += "application/grip-instruct";
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
	}

	void add(RequestSession *rs)
	{
		assert(addAllowed);

		requestSessions += rs;

		if(state == Stopped)
		{
			state = Requesting;

			host = rs->host();
			isHttps = rs->isHttps();

			if(rs->isRetry())
			{
				requestData = rs->retryData();
			}
			else
			{
				m2Request = rs->request();
				connect(m2Request, SIGNAL(readyRead()), SLOT(m2Request_readyRead()));
				connect(m2Request, SIGNAL(finished()), SLOT(m2Request_finished()));
				connect(m2Request, SIGNAL(error()), SLOT(m2Request_error()));

				requestData.method = m2Request->method();
				requestData.path = m2Request->path();
				requestData.headers = m2Request->headers();
				requestData.body = m2Request->read();
			}

			// TODO: support multiple targets

			QList<DomainMap::Target> targets = domainMap->entry(host);
			log_debug("%s has %d routes", qPrintable(host), targets.count());
			QByteArray str = "http://" + targets[0].first.toUtf8() + ':' + QByteArray::number(targets[0].second) + requestData.path;
			QUrl url(str);

			zurlRequest = zurlManager->createRequest();
			connect(zurlRequest, SIGNAL(readyRead()), SLOT(zurlRequest_readyRead()));
			connect(zurlRequest, SIGNAL(bytesWritten(int)), SLOT(zurlRequest_bytesWritten(int)));
			connect(zurlRequest, SIGNAL(error()), SLOT(zurlRequest_error()));

			zurlRequest->start(requestData.method, url, requestData.headers);

			if(!requestData.body.isEmpty())
				zurlRequest->writeBody(requestData.body);

			if(!m2Request || m2Request->isFinished())
				zurlRequest->endBody();
		}
		else if(state == Requesting)
		{
			// nothing to do, just wait around until a response comes
		}
		else if(state == Responding)
		{
			// get the session caught up with where we're at

			M2Response *resp = rs->createResponse();
			connect(resp, SIGNAL(bytesWritten(int)), SLOT(m2Response_bytesWritten(int)));
			connect(resp, SIGNAL(finished()), SLOT(m2Response_finished()));

			m2Responses += resp;
			sessionsByResponse.insert(resp, rs);

			resp->start(responseData.code, responseData.status, responseData.headers);

			if(!responseData.body.isEmpty())
				resp->write(responseData.body);
		}
	}

public slots:
	void m2Request_readyRead()
	{
		QByteArray buf = m2Request->read();
		log_debug("proxysession: input chunk: %d", buf.size());

		if(requestData.body.size() + buf.size() > MAX_ACCEPT_REQUEST_BODY)
		{
			// TODO: reject all sessions
		}

		requestData.body += buf;
		zurlRequest->writeBody(buf);
	}

	void m2Request_finished()
	{
		log_debug("proxysession: input finished");

		zurlRequest->endBody();
	}

	void m2Request_error()
	{
		log_error("proxysession: input error");

		cleanup();
		emit q->finishedByPassthrough();
	}

	void zurlRequest_readyRead()
	{
		log_debug("zurlRequest_readyRead");

		if(state == Requesting)
		{
			responseData.code = zurlRequest->responseCode();
			responseData.status = zurlRequest->responseStatus();
			responseData.headers = zurlRequest->responseHeaders();
			responseData.body = zurlRequest->readResponseBody(); // initial body chunk

			total += responseData.body.size();
			log_debug("recv total: %d\n", total);

			if(acceptTypes.contains(responseData.headers.get("Content-Type")))
			{
				state = Accepting;
			}
			else
			{
				state = Responding;

				if(!responseData.headers.contains("Content-Length") && !responseData.headers.contains("Transfer-Encoding"))
						responseData.headers += HttpHeader("Transfer-Encoding", "chunked");

				foreach(RequestSession *rs, requestSessions)
				{
					M2Response *resp = rs->createResponse();
					connect(resp, SIGNAL(bytesWritten(int)), SLOT(m2Response_bytesWritten(int)));
					connect(resp, SIGNAL(finished()), SLOT(m2Response_finished()));

					m2Responses += resp;
					sessionsByResponse.insert(resp, rs);

					resp->start(responseData.code, responseData.status, responseData.headers);

					if(!responseData.body.isEmpty())
						resp->write(responseData.body);
				}
			}
		}
		else
		{
			QByteArray buf = zurlRequest->readResponseBody();

			total += buf.size();
			log_debug("recv total: %d", total);

			if(!buf.isEmpty())
			{
				if(state == Accepting)
				{
					if(responseData.body.size() + buf.size() > MAX_ACCEPT_RESPONSE_BODY)
					{
						// TODO: reject all sessions
					}

					responseData.body += buf;
				}
				else // Responding
				{
					bool wasAllowed = addAllowed;

					if(addAllowed)
					{
						if(responseData.body.size() + buf.size() > MAX_ACCEPT_RESPONSE_BODY)
						{
							responseData.body.clear();
							addAllowed = false;
						}
						else
							responseData.body += buf;
					}

					log_debug("writing %d", buf.size());
					foreach(M2Response *resp, m2Responses)
						resp->write(buf);

					if(wasAllowed && !addAllowed)
						emit q->addNotAllowed();
				}
			}
		}

		if(zurlRequest->isFinished())
		{
			log_debug("zurlRequest finished");

			delete zurlRequest;
			zurlRequest = 0;

			if(state == Accepting)
			{
				foreach(RequestSession *rs, requestSessions)
				{
					if(rs->isRetry())
						adata.rids += rs->retryRid();
					else
						adata.rids += rs->request()->rid();
				}

				adata.request = requestData;

				adata.https = isHttps;

				adata.haveResponse = true;
				adata.response = responseData;

				emit q->finishedForAccept(adata);
			}
			else // Responding
			{
				foreach(M2Response *resp, m2Responses)
					resp->close();

				// once the entire reponse has been received, cut off any new adds
				if(addAllowed)
				{
					addAllowed = false;
					emit q->addNotAllowed();
				}
			}
		}
	}

	void zurlRequest_bytesWritten(int count)
	{
		// TODO: flow control
		Q_UNUSED(count);
	}

	void zurlRequest_error()
	{
		log_debug("zurlRequest_error");

		// TODO: reject all sessions
	}

	void m2Response_bytesWritten(int count)
	{
		// TODO: flow control
		Q_UNUSED(count);
	}

	void m2Response_finished()
	{
		M2Response *resp = (M2Response *)sender();

		log_debug("m2Response_finished");

		RequestSession *rs = sessionsByResponse.value(resp);
		assert(rs);

		sessionsByResponse.remove(resp);
		m2Responses.remove(resp);
		delete resp;
		delete rs;

		if(m2Responses.isEmpty())
			emit q->finishedByPassthrough();
	}
};

ProxySession::ProxySession(ZurlManager *zurlManager, DomainMap *domainMap, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, zurlManager, domainMap);
}

ProxySession::~ProxySession()
{
	delete d;
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
