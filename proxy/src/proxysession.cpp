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
#include <QDateTime>
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "log.h"
#include "jwt.h"
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

static QByteArray make_token(const QByteArray &iss, const QByteArray &key)
{
	QVariantMap claim;
	claim["iss"] = QString::fromUtf8(iss);
	claim["exp"] = QDateTime::currentDateTimeUtc().toTime_t() + 3600;
	return Jwt::encode(claim, key);
}

static bool validate_token(const QByteArray &token, const QByteArray &iss, const QByteArray &key)
{
	QVariant claimObj = Jwt::decode(token, key);
	if(!claimObj.isValid() || claimObj.type() != QVariant::Map)
		return false;

	QVariantMap claim = claimObj.toMap();

	if(claim.value("iss").toString().toUtf8() != iss)
		return false;

	int exp = claim.value("exp").toInt();
	if(exp <= 0 || exp >= (int)QDateTime::currentDateTimeUtc().toTime_t())
		return false;

	return true;
}

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
		enum State
		{
			WaitingForResponse,
			Responding,
			Responded,
			Errored
		};

		RequestSession *rs;
		State state;
		int bytesToWrite;

		SessionItem() :
			rs(0),
			state(WaitingForResponse),
			bytesToWrite(0)
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
	QByteArray defaultSigIss;
	QByteArray defaultSigKey;
	QByteArray defaultUpstreamIss;
	QByteArray defaultUpstreamKey;
	bool passToUpstream;

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
		total(0),
		passToUpstream(false)
	{
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

			QList<DomainMap::Target> targets = domainMap->entry(host, requestData.path, isHttps);
			log_debug("proxysession: %p %s has %d routes", q, qPrintable(host), targets.count());
			QByteArray str = targets[0].ssl ? "https://" : "http://";
			str += targets[0].host.toUtf8() + ':' + QByteArray::number(targets[0].port) + requestData.path;
			QUrl url = QUrl::fromEncoded(str, QUrl::StrictMode);

			log_debug("proxysession: %p forwarding to %s", q, url.toEncoded().data());

			// check if the request is coming from a grip proxy already
			if(!defaultUpstreamIss.isEmpty() && !defaultUpstreamKey.isEmpty())
			{
				QByteArray token = requestData.headers.get("Grip-Sig");
				if(!token.isEmpty())
				{
					if(validate_token(token, defaultUpstreamIss, defaultUpstreamKey))
					{
						log_debug("proxysession: %p passing to upstream", q);
						passToUpstream = true;
					}
					else
						log_debug("proxysession: %p signature present but invalid", q);
				}
			}

			if(!passToUpstream)
			{
				// remove/replace Grip-Sig
				requestData.headers.removeAll("Grip-Sig");
				if(!defaultSigIss.isEmpty() && !defaultSigKey.isEmpty())
				{
					QByteArray token = make_token(defaultSigIss, defaultSigKey);
					if(!token.isEmpty())
						requestData.headers += HttpHeader("Grip-Sig", token);
					else
						log_warning("proxysession: %p failed to sign request", q);
				}
			}

			zurlRequest = zurlManager->createRequest();
			zurlRequest->setParent(this);
			connect(zurlRequest, SIGNAL(readyRead()), SLOT(zurlRequest_readyRead()));
			connect(zurlRequest, SIGNAL(bytesWritten(int)), SLOT(zurlRequest_bytesWritten(int)));
			connect(zurlRequest, SIGNAL(error()), SLOT(zurlRequest_error()));

			if(targets[0].trusted)
				zurlRequest->setIgnorePolicies(true);

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

			si->state = SessionItem::Responding;
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

		log_debug("proxysession: %p input chunk: %d", q, buf.size());

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
			if(si->state != SessionItem::Errored)
			{
				assert(si->state == SessionItem::WaitingForResponse);

				si->state = SessionItem::Responded;
				si->bytesToWrite = -1;
				si->rs->respondCannotAccept();
			}
		}
	}

	void rejectAll(int code, const QString &status, const QString &errorMessage)
	{
		foreach(SessionItem *si, sessionItems)
		{
			if(si->state != SessionItem::Errored)
			{
				assert(si->state == SessionItem::WaitingForResponse);

				si->state = SessionItem::Responded;
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
			assert(si->state != SessionItem::WaitingForResponse);

			if(si->state == SessionItem::Responding)
			{
				si->state = SessionItem::Responded;
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

		QPointer<QObject> self = this;

		QByteArray buf = zurlRequest->readResponseBody(MAX_STREAM_BUFFER);
		if(!buf.isEmpty())
		{
			total += buf.size();
			log_debug("proxysession: %p recv=%d, total=%d", q, buf.size(), total);

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
					emit q->addNotAllowed();
					if(!self)
						return;
				}
			}
		}

		if(zurlRequest->isFinished())
		{
			log_debug("proxysession: %p response from target finished", q);

			if(!buffering && pendingWrites())
			{
				log_debug("proxysession: %p still stuff left to write, though. we'll wait.", q);
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

				log_debug("proxysession: %p finished for accept", q);
				cleanup();
				emit q->finishedForAccept(adata);
			}
			else // Responding
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

				// once the entire response has been received, cut off any new adds
				if(addAllowed)
				{
					addAllowed = false;
					emit q->addNotAllowed();
				}
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
		log_debug("proxysession: %p finished reading request", q);

		zurlRequest->endBody();
	}

	void m2Request_error()
	{
		log_warning("proxysession: %p error reading request", q);

		rejectAll(500, "Internal Server Error", "Primary shared request failed.");
	}

	void zurlRequest_readyRead()
	{
		log_debug("proxysession: %p data from target", q);

		if(state == Requesting)
		{
			responseData.code = zurlRequest->responseCode();
			responseData.status = zurlRequest->responseStatus();
			responseData.headers = zurlRequest->responseHeaders();
			responseData.body = zurlRequest->readResponseBody(MAX_INITIAL_BUFFER);

			total += responseData.body.size();
			log_debug("proxysession: %p recv total: %d", q, total);

			if(!passToUpstream && acceptTypes.contains(responseData.headers.get("Content-Type")))
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

					si->state = SessionItem::Responding;
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

			tryResponseRead();
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
		ZurlRequest::ErrorCondition e = zurlRequest->errorCondition();
		log_debug("proxysession: %p target error state=%d, condition=%d", q, (int)state, (int)e);

		if(state == Requesting || state == Accepting)
		{
			switch(e)
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

		log_debug("proxysession: %p response bytes written id=%s: %d", q, rs->rid().second.data(), count);

		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		if(si->bytesToWrite != -1)
		{
			si->bytesToWrite -= count;
			assert(si->bytesToWrite >= 0);
		}

		// everyone caught up? try to read some more then
		if(!buffering && zurlRequest && !pendingWrites())
			tryResponseRead();
	}

	void rs_finished()
	{
		RequestSession *rs = (RequestSession *)sender();

		log_debug("proxysession: %p response finished id=%s", q, rs->rid().second.data());

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
		{
			log_debug("proxysession: %p finished by passthrough", q);
			emit q->finishedByPassthrough();
		}
	}

	void rs_errorResponding()
	{
		RequestSession *rs = (RequestSession *)sender();

		log_debug("proxysession: %p response error id=%s", q, rs->rid().second.data());

		SessionItem *si = sessionItemsBySession.value(rs);
		assert(si);

		assert(si->state != SessionItem::Errored);

		// flag that we should stop attempting to respond
		si->state = SessionItem::Errored;
		si->bytesToWrite = -1;

		// don't destroy the RequestSession here. a finished signal will arrive next.
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

void ProxySession::setDefaultSigKey(const QByteArray &iss, const QByteArray &key)
{
	d->defaultSigIss = iss;
	d->defaultSigKey = key;
}

void ProxySession::setDefaultUpstreamKey(const QByteArray &iss, const QByteArray &key)
{
	d->defaultUpstreamIss = iss;
	d->defaultUpstreamKey = key;
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
