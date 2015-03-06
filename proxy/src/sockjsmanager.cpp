/*
 * Copyright (C) 2015 Fanout, Inc.
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

#include "sockjsmanager.h"

#include <assert.h>
#include <QtCrypto>
#include <qjson/serializer.h>
#include "log.h"
#include "bufferlist.h"
#include "packet/httpresponsedata.h"
#include "zhttprequest.h"
#include "zwebsocket.h"
#include "sockjssession.h"

#define MAX_REQUEST_BODY 100000

class SockJsManager::Private : public QObject
{
	Q_OBJECT

public:
	class RequestItem
	{
	public:
		ZhttpRequest *req;
		BufferList in;
		QByteArray path;
		QUrl asUri;
		DomainMap::Entry route;

		RequestItem(ZhttpRequest *_req) :
			req(_req)
		{
		}

		~RequestItem()
		{
			delete req;
		}
	};

	class SocketItem
	{
	public:
		ZWebSocket *sock;
		QByteArray path;
		QUrl asUri;
		DomainMap::Entry route;

		SocketItem(ZWebSocket *_sock) :
			sock(_sock)
		{
		}

		~SocketItem()
		{
			delete sock;
		}
	};

	SockJsManager *q;
	QHash<ZhttpRequest*, RequestItem*> requestItemsByRequest;
	QHash<ZWebSocket*, SocketItem*> socketItemsBySocket;
	QHash<QByteArray, SockJsSession*> sessionsById;
	QList<SockJsSession*> pendingSessions;

	Private(SockJsManager *_q) :
		QObject(_q),
		q(_q)
	{
	}

	~Private()
	{
		qDeleteAll(requestItemsByRequest);
	}

	RequestItem *track(ZhttpRequest *req)
	{
		assert(!requestItemsByRequest.contains(req));

		connect(req, SIGNAL(readyRead()), SLOT(req_readyRead()));
		connect(req, SIGNAL(bytesWritten(int)), SLOT(req_bytesWritten(int)));
		connect(req, SIGNAL(error()), SLOT(req_error()));

		RequestItem *ri = new RequestItem(req);
		requestItemsByRequest.insert(req, ri);
		return ri;
	}

	SocketItem *track(ZWebSocket *sock)
	{
		assert(!socketItemsBySocket.contains(sock));

		connect(sock, SIGNAL(closed()), SLOT(sock_closed()));
		connect(sock, SIGNAL(error()), SLOT(sock_error()));

		SocketItem *si = new SocketItem(sock);
		socketItemsBySocket.insert(sock, si);
		return si;
	}

	void removeItem(RequestItem *ri)
	{
		requestItemsByRequest.remove(ri->req);
		delete ri;
	}

	void removeItem(SocketItem *si)
	{
		socketItemsBySocket.remove(si->sock);
		delete si;
	}

	void startHandleRequest(ZhttpRequest *req, int basePathStart, const QByteArray &asPath, const DomainMap::Entry &route)
	{
		RequestItem *ri = track(req);

		QByteArray encPath = req->requestUri().encodedPath();
		ri->path = encPath.mid(basePathStart);
		ri->asUri = req->requestUri();
		ri->asUri.setScheme((ri->asUri.scheme() == "https") ? "wss" : "ws");
		if(!asPath.isEmpty())
			ri->asUri.setEncodedPath(asPath);
		else
			ri->asUri.setEncodedPath(encPath.mid(0, basePathStart) + "/websocket");
		ri->route = route;

		processRequestInput(ri);
	}

	void startHandleSocket(ZWebSocket *sock, int basePathStart, const QByteArray &asPath, const DomainMap::Entry &route)
	{
		QByteArray encPath = sock->requestUri().encodedPath();
		QByteArray path = encPath.mid(basePathStart);
		QUrl asUri = sock->requestUri();
		if(!asPath.isEmpty())
			asUri.setEncodedPath(asPath);
		else
			asUri.setEncodedPath(encPath.mid(0, basePathStart) + "/websocket");

		handleSocket(sock, path, asUri, route);
	}

	void applyHeaders(const HttpHeaders &in, HttpHeaders *out)
	{
		*out += HttpHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");

		QByteArray origin;
		if(in.contains("Origin"))
			origin = in.get("Origin");
		else
			origin = "*";
		*out += HttpHeader("Access-Control-Allow-Origin", origin);
		*out += HttpHeader("Access-Control-Allow-Credentials", "true");
	}

	void respond(RequestItem *ri, int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		if(ri->req->isErrored())
		{
			removeItem(ri);
			return;
		}

		ri->req->beginResponse(code, reason, headers);
		ri->req->writeBody(body);
		ri->req->endBody();
	}

	void respondError(RequestItem *ri, int code, const QByteArray &reason, const QString &message)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		applyHeaders(ri->req->requestHeaders(), &headers);
		respond(ri, code, reason, headers, message.toUtf8() + '\n');
	}

	void respondError(SocketItem *si, int code, const QByteArray &reason, const QString &message)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		applyHeaders(si->sock->requestHeaders(), &headers);
		si->sock->respondError(code, reason, headers, message.toUtf8() + '\n');
	}

	void respond(ZhttpRequest *req, int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		RequestItem *ri = track(req);
		respond(ri, code, reason, headers, body);
	}

	void respondEmpty(ZhttpRequest *req)
	{
		RequestItem *ri = track(req);

		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain"); // workaround FF issue. see sockjs spec.
		applyHeaders(ri->req->requestHeaders(), &headers);

		respond(ri, 204, "No Content", headers, QByteArray());
	}

	void respondOk(ZhttpRequest *req, const QByteArray &contentType, const QByteArray &body = QByteArray())
	{
		RequestItem *ri = track(req);

		HttpHeaders headers;
		headers += HttpHeader("Content-Type", contentType);
		applyHeaders(ri->req->requestHeaders(), &headers);

		respond(ri, 200, "OK", headers, body);
	}

	void respondOk(ZhttpRequest *req, const QVariantMap &data)
	{
		RequestItem *ri = track(req);

		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "application/json");
		applyHeaders(ri->req->requestHeaders(), &headers);

		QJson::Serializer serializer;
		QByteArray bodyJson = serializer.serialize(data);
		respond(ri, 200, "OK", headers, bodyJson + "\n");
	}

	void respondError(ZhttpRequest *req, int code, const QByteArray &reason, const QString &message)
	{
		RequestItem *ri = track(req);
		respondError(ri, code, reason, message);
	}

	void respondError(ZWebSocket *sock, int code, const QByteArray &reason, const QString &message)
	{
		SocketItem *si = track(sock);
		respondError(si, code, reason, message);
	}

	void processRequestInput(RequestItem *ri)
	{
		ri->in += ri->req->readBody(MAX_REQUEST_BODY - ri->in.size() + 1);
		if(ri->in.size() > MAX_REQUEST_BODY)
		{
			respondError(ri, 400, "Bad Request", "Request too large.");
			return;
		}

		if(ri->req->isInputFinished())
		{
			ZhttpRequest *req = ri->req;
			QByteArray body = ri->in.toByteArray();
			QByteArray path = ri->path;
			QUrl asUri = ri->asUri;
			DomainMap::Entry route = ri->route;

			requestItemsByRequest.remove(ri->req);
			ri->req = 0;
			delete ri;
			req->disconnect(this);

			handleRequest(req, path, asUri, body, route);
		}
	}

	void handleRequest(ZhttpRequest *req, const QByteArray &path, const QUrl &asUri, const QByteArray &body, const DomainMap::Entry &route)
	{
		QString method = req->requestMethod();
		log_debug("sockjs request: path=[%s], asUri=[%s]", path.data(), asUri.toEncoded().data());

		if(method == "OPTIONS")
		{
			respondEmpty(req);
		}
		else if(path == "/info")
		{
			QByteArray bytes = QCA::Random::randomArray(4).toByteArray();
			quint32 x = 0;
			for(int n = 0; n < 4; ++n)
				x |= ((unsigned char)bytes[n]) << ((3-n) * 8);

			QVariantMap out;
			out["websocket"] = true;
			out["origins"] = QVariantList() << QString("*:*");
			out["cookie_needed"] = false;
			out["entropy"] = x;
			respondOk(req, out);
		}
		else
		{
			QList<QByteArray> parts = path.mid(1).split('/');
			if(parts.count() == 3)
			{
				QByteArray sid = parts[1];
				QByteArray lastPart = parts[2];

				SockJsSession *sess = sessionsById.value(sid);
				if(sess)
				{
					sess->handleRequest(req, lastPart, body);
					return;
				}

				// TODO: check for lingering sid

				if(lastPart == "xhr" || lastPart == "jsonp")
				{
					sess = new SockJsSession;
					sess->setupServer(q, req, asUri, sid, lastPart, body, route);
					sessionsById.insert(sid, sess);
					pendingSessions += sess;
					emit q->sessionReady();
					return;
				}
			}

			respondError(req, 404, "Not Found", "Not Found");
		}
	}

	void handleSocket(ZWebSocket *sock, const QByteArray &path, const QUrl &asUri, const DomainMap::Entry &route)
	{
		if(path == "/websocket")
		{
			// passthrough
			SockJsSession *sess = new SockJsSession;
			sess->setupServer(q, sock, asUri, route);
			pendingSessions += sess;
			emit q->sessionReady();
			return;
		}
		else
		{
			QList<QByteArray> parts = path.mid(1).split('/');
			if(parts.count() == 3)
			{
				QByteArray sid = parts[1];
				QByteArray lastPart = parts[2];

				SockJsSession *sess = new SockJsSession;
				sess->setupServer(q, sock, asUri, sid, lastPart, route);
				pendingSessions += sess;
				emit q->sessionReady();
				return;
			}

			respondError(sock, 404, "Not Found", "Not Found");
		}
	}

private slots:
	void req_readyRead()
	{
		ZhttpRequest *req = (ZhttpRequest *)sender();
		RequestItem *ri = requestItemsByRequest.value(req);
		assert(ri);

		processRequestInput(ri);
	}

	void req_bytesWritten(int count)
	{
		Q_UNUSED(count);

		ZhttpRequest *req = (ZhttpRequest *)sender();
		RequestItem *ri = requestItemsByRequest.value(req);
		assert(ri);

		if(req->isFinished())
			removeItem(ri);
	}

	void req_error()
	{
		ZhttpRequest *req = (ZhttpRequest *)sender();
		RequestItem *ri = requestItemsByRequest.value(req);
		assert(ri);

		removeItem(ri);
	}

	void sock_closed()
	{
		ZWebSocket *sock = (ZWebSocket *)sender();
		SocketItem *si = socketItemsBySocket.value(sock);
		assert(si);

		removeItem(si);
	}

	void sock_error()
	{
		ZWebSocket *sock = (ZWebSocket *)sender();
		SocketItem *si = socketItemsBySocket.value(sock);
		assert(si);

		removeItem(si);
	}
};

SockJsManager::SockJsManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

SockJsManager::~SockJsManager()
{
	delete d;
}

void SockJsManager::giveRequest(ZhttpRequest *req, int basePathStart, const QByteArray &asPath, const DomainMap::Entry &route)
{
	d->startHandleRequest(req, basePathStart, asPath, route);
}

void SockJsManager::giveSocket(ZWebSocket *sock, int basePathStart, const QByteArray &asPath, const DomainMap::Entry &route)
{
	d->startHandleSocket(sock, basePathStart, asPath, route);
}

SockJsSession *SockJsManager::takeNext()
{
	SockJsSession *sess = 0;

	while(!sess)
	{
		if(d->pendingSessions.isEmpty())
			return 0;

		sess = d->pendingSessions.takeFirst();
		// FIXME
		/*if(!d->sessionsById.contains(sess->sid()))
		{
			// this means the object was a zombie. clean up and take next
			delete sess;
			sess = 0;
			continue;
		}*/
	}

	sess->startServer();
	return sess;
}

void SockJsManager::link(SockJsSession *sess)
{
	QByteArray sid = sess->sid();
	assert(!sid.isEmpty());
	d->sessionsById.insert(sid, sess);
}

void SockJsManager::unlink(SockJsSession *sess)
{
	QByteArray sid = sess->sid();
	assert(!sid.isEmpty());
	d->sessionsById.remove(sid);
}

void SockJsManager::respond(ZhttpRequest *req, const HttpResponseData &respData)
{
	d->respond(req, respData.code, respData.reason, respData.headers, respData.body);
}

#include "sockjsmanager.moc"
