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

#include "proxysession.h"

#include <assert.h>
#include <QSet>
#include <QPointer>
#include <QUrl>
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "log.h"
#include "inspectdata.h"
#include "acceptdata.h"
#include "m2request.h"
#include "zurlmanager.h"
#include "zurlrequest.h"
#include "domainmap.h"
#include "requestsession.h"

#define MAX_ACCEPT_REQUEST_BODY 100000
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
		Responding
	};

	class SessionItem
	{
	public:
		RequestSession *rs;
		int bytesToWrite;
		bool responseSent;
		bool errored;

		SessionItem() :
			rs(0),
			bytesToWrite(0),
			responseSent(false),
			errored(false)
		{
		}
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
	QSet<QByteArray> acceptTypes;
	QSet<SessionItem*> sessionItems;
	HttpRequestData requestData;
	HttpResponseData responseData;
	QHash<RequestSession*, SessionItem*> sessionItemsBySession;
	int requestBytesToWrite;
	int total;
	bool buffering;

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
		haveInspectData(false),
		requestBytesToWrite(0),
		total(0)
	{
		acceptTypes += "application/fo-instruct";
		acceptTypes += "application/grip-instruct";
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		foreach(SessionItem *si, sessionItems)
			delete si->rs;

		sessionItems.clear();
		sessionItemsBySession.clear();
	}

	void add(RequestSession *rs)
	{
		assert(addAllowed);

		SessionItem *si = new SessionItem;
		si->rs = rs;
		si->rs->setParent(this);
		sessionItems += si;

		if(state == Stopped)
		{
			state = Requesting;
			buffering = true;

			host = rs->host();
			isHttps = rs->isHttps();

			requestData = rs->requestData();

			// don't relay these headers
			requestData.headers.removeAll("connection");
			requestData.headers.removeAll("accept-encoding");
			requestData.headers.removeAll("content-encoding");
			requestData.headers.removeAll("transfer-encoding");

			if(!rs->isRetry())
			{
				m2Request = rs->request();
				connect(m2Request, SIGNAL(readyRead()), SLOT(m2Request_readyRead()));
				connect(m2Request, SIGNAL(finished()), SLOT(m2Request_finished()));
				connect(m2Request, SIGNAL(error()), SLOT(m2Request_error()));

				requestData.body += m2Request->read();
			}

			QByteArray buf = requestData.body;

			if(requestData.body.size() > MAX_ACCEPT_REQUEST_BODY)
			{
				requestData.body.clear();
				buffering = false;
			}

			// TODO: support multiple targets

			QList<DomainMap::Target> targets = domainMap->entry(host);
			log_debug("%s has %d routes", qPrintable(host), targets.count());
			QByteArray str = "http://" + targets[0].first.toUtf8() + ':' + QByteArray::number(targets[0].second) + requestData.path;
			QUrl url(str);

			log_debug("proxying to %s", qPrintable(url.toString()));

			zurlRequest = zurlManager->createRequest();
			zurlRequest->setParent(this);
			connect(zurlRequest, SIGNAL(readyRead()), SLOT(zurlRequest_readyRead()));
			connect(zurlRequest, SIGNAL(bytesWritten(int)), SLOT(zurlRequest_bytesWritten(int)));
			connect(zurlRequest, SIGNAL(error()), SLOT(zurlRequest_error()));

			zurlRequest->start(requestData.method, url, requestData.headers);

			if(!requestData.body.isEmpty())
			{
				requestBytesToWrite += buf.size();
				zurlRequest->writeBody(buf);
			}

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

			connect(rs, SIGNAL(bytesWritten(int)), SLOT(rs_bytesWritten(int)));
			connect(rs, SIGNAL(errorResponding()), SLOT(rs_errorResponding()));
			connect(rs, SIGNAL(finished()), SLOT(rs_finished()));

			sessionItemsBySession.insert(rs, si);

			rs->startResponse(responseData.code, responseData.status, responseData.headers);

			if(!responseData.body.isEmpty())
			{
				si->bytesToWrite += responseData.body.size();
				rs->writeResponseBody(responseData.body);
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

	void tryRequestRead()
	{
		QByteArray buf = m2Request->read();
		if(buf.isEmpty())
			return;

		log_debug("proxysession: input chunk: %d", buf.size());

		if(buffering)
		{
			if(requestData.body.size() + buf.size() > MAX_ACCEPT_REQUEST_BODY)
			{
				requestData.body.clear();
				buffering = false;
			}
			else
				requestData.body += buf;
		}

		requestBytesToWrite += buf.size();
		zurlRequest->writeBody(buf);
	}

	void cannotAcceptAll()
	{
		foreach(SessionItem *si, sessionItems)
		{
			if(!si->errored)
			{
				si->bytesToWrite = -1;
				si->rs->respondCannotAccept();
			}
		}
	}

	void rejectAll(int code, const QString &status, const QString &errorMessage)
	{
		foreach(SessionItem *si, sessionItems)
		{
			if(!si->errored)
			{
				si->bytesToWrite = -1;
				si->rs->respondError(code, status, errorMessage);
			}
		}
	}

	void destroyAll()
	{
		// this method is only to be called when we are in Responding state
		assert(state == Responding);

		foreach(SessionItem *si, sessionItems)
		{
			if(!si->errored && !si->responseSent)
			{
				si->bytesToWrite = -1;
				si->responseSent = true;
				si->rs->endResponseBody();
			}
		}
	}

public slots:
	void m2Request_readyRead()
	{
		tryRequestRead();
	}

	void m2Request_finished()
	{
		log_debug("proxysession: finished reading request");

		zurlRequest->endBody();
	}

	void m2Request_error()
	{
		log_warning("proxysession: error reading request");

		rejectAll(500, "Internal Server Error", "Primary shared request failed.");
	}

	void zurlRequest_readyRead()
	{
		log_debug("zurlRequest_readyRead");

		QPointer<QObject> self = this;

		if(state == Requesting)
		{
			responseData.code = zurlRequest->responseCode();
			responseData.status = zurlRequest->responseStatus();
			responseData.headers = zurlRequest->responseHeaders();
			responseData.body = zurlRequest->readResponseBody(MAX_INITIAL_BUFFER);

			total += responseData.body.size();
			log_debug("recv total: %d", total);

			if(acceptTypes.contains(responseData.headers.get("Content-Type")))
			{
				if(!buffering)
				{
					rejectAll(502, "Bad Gateway", "Request too large to accept GRIP instruct.");
					return;
				}

				state = Accepting;
			}
			else
			{
				state = Responding;

				// don't relay these headers. zurl deals with their meaning for us.
				responseData.headers.removeAll("connection");
				responseData.headers.removeAll("content-encoding");
				responseData.headers.removeAll("transfer-encoding");

				if(!responseData.headers.contains("Content-Length") && !responseData.headers.contains("Transfer-Encoding"))
					responseData.headers += HttpHeader("Transfer-Encoding", "chunked");

				foreach(SessionItem *si, sessionItems)
				{
					connect(si->rs, SIGNAL(bytesWritten(int)), SLOT(rs_bytesWritten(int)));
					connect(si->rs, SIGNAL(finished()), SLOT(rs_finished()));
					connect(si->rs, SIGNAL(errorResponding()), SLOT(rs_errorResponding()));

					sessionItemsBySession.insert(si->rs, si);

					si->rs->startResponse(responseData.code, responseData.status, responseData.headers);

					if(!responseData.body.isEmpty())
					{
						si->bytesToWrite += responseData.body.size();
						si->rs->writeResponseBody(responseData.body);
					}
				}
			}
		}
		else
		{
			assert(state == Accepting || state == Responding);

			// if we're not buffering, then sync to slowest receiver
			if(!buffering && pendingWrites())
				return;

			QByteArray buf = zurlRequest->readResponseBody(MAX_STREAM_BUFFER);

			total += buf.size();
			log_debug("recv=%d, total=%d", buf.size(), total);

			if(!buf.isEmpty())
			{
				if(state == Accepting)
				{
					if(responseData.body.size() + buf.size() > MAX_ACCEPT_RESPONSE_BODY)
					{
						rejectAll(502, "Bad Gateway", "GRIP instruct response too large.");
						return;
					}

					responseData.body += buf;
				}
				else // Responding
				{
					bool wasAllowed = addAllowed;

					if(buffering)
					{
						if(responseData.body.size() + buf.size() > MAX_INITIAL_BUFFER)
						{
							responseData.body.clear();
							buffering = false;
							addAllowed = false;
						}
						else
							responseData.body += buf;
					}

					log_debug("writing %d", buf.size());
					foreach(SessionItem *si, sessionItems)
					{
						if(!si->errored && !si->responseSent)
						{
							si->bytesToWrite += buf.size();
							si->rs->writeResponseBody(buf);
						}
					}

					if(wasAllowed && !addAllowed)
					{
						emit q->addNotAllowed();
						if(!self)
							return;
					}
				}
			}
		}

		if(zurlRequest->isFinished())
		{
			log_debug("zurlRequest finished");

			if(!buffering && pendingWrites())
			{
				log_debug("still stuff left to write, though. we'll wait.");
				return;
			}

			delete zurlRequest;
			zurlRequest = 0;

			if(state == Accepting)
			{
				AcceptData adata;

				foreach(SessionItem *si, sessionItems)
				{
					AcceptData::Request areq;
					areq.rid = si->rs->rid();
					areq.https = si->rs->isHttps();
					areq.jsonpCallback = si->rs->jsonpCallback();
					adata.requests += areq;
				}

				adata.requestData = requestData;

				adata.haveResponse = true;
				adata.response = responseData;

				cleanup();
				emit q->finishedForAccept(adata);
			}
			else // Responding
			{
				foreach(SessionItem *si, sessionItems)
				{
					if(!si->errored && !si->responseSent)
					{
						si->responseSent = true;
						si->rs->endResponseBody();
					}
				}

				// once the entire response has been received, cut off any new adds
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
		requestBytesToWrite -= count;
		assert(requestBytesToWrite >= 0);

		if(requestBytesToWrite == 0)
			tryRequestRead();
	}

	void zurlRequest_error()
	{
		log_debug("zurlRequest_error");

		if(state == Requesting || state == Accepting)
		{
			switch(zurlRequest->errorCondition())
			{
				case ZurlRequest::ErrorLengthRequired:
					rejectAll(411, "Length Required", "Must provide Content-Length header.");
					break;
				default:
					rejectAll(502, "Bad Gateway", "Error while proxying to origin.");
					break;
			}
		}
		else if(state == Responding)
		{
			// if we're already responding, then we can't reply with an error
			destroyAll();
		}
	}

	void rs_bytesWritten(int count)
	{
		RequestSession *rs = (RequestSession *)sender();

		log_debug("rs_bytesWritten: %d", count);

		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		if(si->bytesToWrite != -1)
		{
			si->bytesToWrite -= count;
			assert(si->bytesToWrite >= 0);
		}

		// everyone caught up? try to read some more then
		if(!buffering && zurlRequest && !pendingWrites())
			zurlRequest_readyRead();
	}

	void rs_finished()
	{
		RequestSession *rs = (RequestSession *)sender();

		log_debug("rs_finished");

		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		QPointer<QObject> self = this;
		emit q->requestSessionDestroyed(si->rs);
		if(!self)
			return;

		sessionItemsBySession.remove(rs);
		sessionItems.remove(si);
		delete rs;

		delete si;

		if(sessionItems.isEmpty())
			emit q->finishedByPassthrough();
	}

	void rs_errorResponding()
	{
		RequestSession *rs = (RequestSession *)sender();

		log_debug("rs_errorResponding");

		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		// flag that we should stop attempting to respond
		si->errored = true;
		si->bytesToWrite = -1;
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

void ProxySession::cannotAccept()
{
	d->cannotAcceptAll();
}

#include "proxysession.moc"
