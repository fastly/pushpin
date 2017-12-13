/*
 * Copyright (C) 2015-2017 Fanout, Inc.
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

#include "sockjsmanager.h"

#include <assert.h>
#include <QtGlobal>
#include <QTimer>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include "log.h"
#include "bufferlist.h"
#include "zhttprequest.h"
#include "zwebsocket.h"
#include "sockjssession.h"

#define MAX_REQUEST_BODY 100000

const char *iframeHtmlTemplate =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"  <meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\" />\n"
"  <meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n"
"  <script src=\"%1\"></script>\n"
"  <script>\n"
"    document.domain = document.domain;\n"
"    SockJS.bootstrap_iframe();\n"
"  </script>\n"
"</head>\n"
"<body>\n"
"  <h2>Don't panic!</h2>\n"
"  <p>This is a SockJS hidden iframe. It's used for cross domain magic.</p>\n"
"</body>\n"
"</html>\n";

static QByteArray serializeJsonString(const QString &s)
{
	QByteArray tmp = QJsonDocument(QJsonArray::fromVariantList(QVariantList() << s)).toJson(QJsonDocument::Compact);

	assert(tmp.length() >= 4);
	assert(tmp[0] == '[' && tmp[tmp.length() - 1] == ']');
	assert(tmp[1] == '"' && tmp[tmp.length() - 2] == '"');

	return tmp.mid(1, tmp.length() - 2);
}

class SockJsManager::Private : public QObject
{
	Q_OBJECT

public:
	class Session
	{
	public:
		enum Type
		{
			Http,
			WebSocket
		};

		Private *owner;
		Type type;
		ZhttpRequest *req;
		ZWebSocket *sock;
		BufferList reqBody;
		QByteArray path;
		QByteArray jsonpCallback;
		QUrl asUri;
		DomainMap::Entry route;
		QByteArray sid;
		QByteArray lastPart;
		bool pending;
		SockJsSession *ext;
		QTimer *timer;
		QVariant closeValue;

		Session(Private *_owner) :
			owner(_owner),
			req(0),
			sock(0),
			pending(false),
			ext(0),
			timer(0)
		{
		}

		~Session()
		{
			delete req;
			delete sock;

			if(timer)
			{
				timer->disconnect(owner);
				timer->setParent(0);
				timer->deleteLater();
			}
		}
	};

	SockJsManager *q;
	QSet<Session*> sessions;
	QHash<ZhttpRequest*, Session*> sessionsByRequest;
	QHash<ZWebSocket*, Session*> sessionsBySocket;
	QHash<QByteArray, Session*> sessionsById;
	QHash<SockJsSession*, Session*> sessionsByExt;
	QHash<QTimer*, Session*> sessionsByTimer;
	QList<Session*> pendingSessions;
	QByteArray iframeHtml;
	QByteArray iframeHtmlEtag;
	QSet<ZhttpRequest*> discardedRequests;

	Private(SockJsManager *_q, const QString &sockJsUrl) :
		QObject(_q),
		q(_q)
	{
		iframeHtml = QString(iframeHtmlTemplate).arg(sockJsUrl).toUtf8();
		iframeHtmlEtag = '\"' + QCryptographicHash::hash(iframeHtml, QCryptographicHash::Md5).toHex() + '\"';
	}

	~Private()
	{
		qDeleteAll(discardedRequests);

		while(!pendingSessions.isEmpty())
			removeSession(pendingSessions.takeFirst());

		assert(sessions.isEmpty());
	}

	void removeSession(Session *s)
	{
		// can't remove unless unlinked
		assert(!s->ext);

		// note: this method assumes the session has already been removed
		//   from pendingSessions if needed
		if(s->req)
			sessionsByRequest.remove(s->req);
		if(s->sock)
			sessionsBySocket.remove(s->sock);
		if(!s->sid.isEmpty())
			sessionsById.remove(s->sid);
		if(s->ext)
			sessionsByExt.remove(s->ext);
		if(s->timer)
			sessionsByTimer.remove(s->timer);
		sessions.remove(s);
		delete s;
	}

	void unlink(SockJsSession *ext)
	{
		Session *s = sessionsByExt.value(ext);
		assert(s);

		sessionsByExt.remove(s->ext);
		s->ext = 0;

		if(s->closeValue.isValid())
		{
			// if there's a close value, hang around for a little bit
			s->timer = new QTimer(this);
			connect(s->timer, &QTimer::timeout, this, &Private::timer_timeout);
			s->timer->setSingleShot(true);
			sessionsByTimer.insert(s->timer, s);
			s->timer->start(5000);
		}
		else
			removeSession(s);
	}

	void setLinger(SockJsSession *ext, const QVariant &closeValue)
	{
		Session *s = sessionsByExt.value(ext);
		assert(s);

		s->closeValue = closeValue;
	}

	void startHandleRequest(ZhttpRequest *req, int basePathStart, const QByteArray &asPath, const DomainMap::Entry &route)
	{
		Session *s = new Session(this);
		s->req = req;

		QUrl uri = req->requestUri();

		QByteArray encPath = uri.path(QUrl::FullyEncoded).toUtf8();
		s->path = encPath.mid(basePathStart);

		QUrlQuery query(uri);

		QList<QByteArray> parts = s->path.split('/');
		if(!parts.isEmpty() && parts.last().startsWith("jsonp"))
		{
			if(query.hasQueryItem("callback"))
			{
				s->jsonpCallback = query.queryItemValue("callback").toUtf8();
				query.removeAllQueryItems("callback");
			}
			else if(query.hasQueryItem("c"))
			{
				s->jsonpCallback = query.queryItemValue("c").toUtf8();
				query.removeAllQueryItems("c");
			}
		}

		s->asUri = uri;
		s->asUri.setScheme((s->asUri.scheme() == "https") ? "wss" : "ws");
		if(!asPath.isEmpty())
			s->asUri.setPath(QString::fromUtf8(asPath), QUrl::StrictMode);
		else
			s->asUri.setPath(QString::fromUtf8(encPath.mid(0, basePathStart)), QUrl::StrictMode);

		s->route = route;

		connect(req, &ZhttpRequest::readyRead, this, &Private::req_readyRead);
		connect(req, &ZhttpRequest::bytesWritten, this, &Private::req_bytesWritten);
		connect(req, &ZhttpRequest::error, this, &Private::req_error);

		sessions += s;
		sessionsByRequest.insert(s->req, s);

		processRequestInput(s);
	}

	void startHandleSocket(ZWebSocket *sock, int basePathStart, const QByteArray &asPath, const DomainMap::Entry &route)
	{
		Session *s = new Session(this);
		s->sock = sock;

		QByteArray encPath = sock->requestUri().path(QUrl::FullyEncoded).toUtf8();
		s->path = encPath.mid(basePathStart);
		s->asUri = sock->requestUri();
		if(!asPath.isEmpty())
			s->asUri.setPath(QString::fromUtf8(asPath), QUrl::StrictMode);
		else
			s->asUri.setPath(QString::fromUtf8(encPath.mid(0, basePathStart) + "/websocket"), QUrl::StrictMode);
		s->route = route;

		connect(sock, &ZWebSocket::closed, this, &Private::sock_closed);
		connect(sock, &ZWebSocket::error, this, &Private::sock_error);

		sessions += s;
		sessionsBySocket.insert(s->sock, s);

		handleSocket(s);
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

	void respond(ZhttpRequest *req, int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		HttpHeaders outHeaders = headers;
		applyHeaders(req->requestHeaders(), &outHeaders);

		req->beginResponse(code, reason, outHeaders);
		req->writeBody(body);
		req->endBody();
	}

	void respondEmpty(ZhttpRequest *req)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain"); // workaround FF issue. see sockjs spec.
		respond(req, 204, "No Content", headers, QByteArray());
	}

	void respondOk(ZhttpRequest *req, const QVariant &data, const QByteArray &prefix = QByteArray(), const QByteArray &jsonpCallback = QByteArray())
	{
		HttpHeaders headers;
		if(!jsonpCallback.isEmpty())
			headers += HttpHeader("Content-Type", "application/javascript");
		else
			headers += HttpHeader("Content-Type", "text/plain");

		QByteArray body;
		if(data.isValid())
		{
			QJsonDocument doc;
			if(data.type() == QVariant::Map)
				doc = QJsonDocument(QJsonObject::fromVariantMap(data.toMap()));
			else // List
				doc = QJsonDocument(QJsonArray::fromVariantList(data.toList()));

			body = doc.toJson(QJsonDocument::Compact);
		}
		if(!prefix.isEmpty())
			body.prepend(prefix);

		if(!jsonpCallback.isEmpty())
		{
			QByteArray encBody = serializeJsonString(QString::fromUtf8(body));
			body = "/**/" + jsonpCallback + '(' + encBody + ");\n";
		}
		else if(!body.isEmpty())
			body += "\n"; // newline is required

		respond(req, 200, "OK", headers, body);
	}

	void respondOk(ZhttpRequest *req, const QString &str, const QByteArray &jsonpCallback = QByteArray())
	{
		HttpHeaders headers;
		if(!jsonpCallback.isEmpty())
			headers += HttpHeader("Content-Type", "application/javascript");
		else
			headers += HttpHeader("Content-Type", "text/plain");

		QByteArray body;
		if(!jsonpCallback.isEmpty())
		{
			QByteArray encBody = serializeJsonString(str);
			body = "/**/" + jsonpCallback + '(' + encBody + ");\n";
		}
		else
			body = str.toUtf8();

		respond(req, 200, "OK", headers, body);
	}

	void respondError(ZhttpRequest *req, int code, const QByteArray &reason, const QString &message, bool discard = false)
	{
		// if discarded, manager takes ownership of req to handle sending
		if(discard)
		{
			discardedRequests += req;

			connect(req, &ZhttpRequest::readyRead, this, &Private::req_readyRead);
			connect(req, &ZhttpRequest::bytesWritten, this, &Private::req_bytesWritten);
			connect(req, &ZhttpRequest::error, this, &Private::req_error);
		}

		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		respond(req, code, reason, headers, message.toUtf8() + '\n');
	}

	void respondError(ZWebSocket *sock, int code, const QByteArray &reason, const QString &message)
	{
		HttpHeaders headers;
		headers += HttpHeader("Content-Type", "text/plain");
		sock->respondError(code, reason, headers, message.toUtf8() + '\n');
	}

	void processRequestInput(Session *s)
	{
		s->reqBody += s->req->readBody(MAX_REQUEST_BODY - s->reqBody.size() + 1);
		if(s->reqBody.size() > MAX_REQUEST_BODY)
		{
			respondError(s->req, 400, "Bad Request", "Request too large.");
			return;
		}

		if(s->req->isInputFinished())
			handleRequest(s);
	}

	void handleRequest(Session *s)
	{
		QString method = s->req->requestMethod();
		log_debug("sockjs request: path=[%s], asUri=[%s]", s->path.data(), s->asUri.toEncoded().data());

		if(method == "OPTIONS")
		{
			respondEmpty(s->req);
		}
		else if(method == "GET" && s->path == "/info")
		{
			quint32 x = (quint32)qrand();

			QVariantMap out;
			out["websocket"] = true;
			out["origins"] = QVariantList() << QString("*:*");
			out["cookie_needed"] = false;
			out["entropy"] = x;
			respondOk(s->req, out);
		}
		else if(method == "GET" && s->path.startsWith("/iframe") && s->path.endsWith(".html"))
		{
			HttpHeaders headers;
			headers += HttpHeader("ETag", iframeHtmlEtag);

			QByteArray ifNoneMatch = s->req->requestHeaders().get("If-None-Match");
			if(ifNoneMatch == iframeHtmlEtag)
			{
				respond(s->req, 304, "Not Modified", headers, QByteArray());
			}
			else
			{
				headers += HttpHeader("Content-Type", "text/html; charset=UTF-8");
				headers += HttpHeader("Cache-Control", "public, max-age=31536000");
				respond(s->req, 200, "OK", headers, iframeHtml);
			}
		}
		else
		{
			QList<QByteArray> parts = s->path.mid(1).split('/');
			if(parts.count() == 3)
			{
				QByteArray sid = parts[1];
				QByteArray lastPart = parts[2];

				Session *existing = sessionsById.value(sid);
				if(existing)
				{
					if(existing->ext)
					{
						// give to external session
						ZhttpRequest *req = s->req;
						QByteArray body = s->reqBody.toByteArray();
						QByteArray jsonpCallback = s->jsonpCallback;
						s->req->disconnect(this);
						s->req = 0;
						removeSession(s);

						existing->ext->handleRequest(req, jsonpCallback, lastPart, body);
					}
					else
					{
						if(existing->closeValue.isValid())
						{
							respondOk(s->req, existing->closeValue, "c", s->jsonpCallback);
						}
						else
						{
							QVariantList out;
							out += 2010;
							out += QString("Another connection still open");
							respondOk(s->req, out, "c", s->jsonpCallback);
						}
					}
					return;
				}

				if((method == "POST" && lastPart == "xhr") || ((method == "GET" || method == "POST") && lastPart == "jsonp"))
				{
					if(lastPart == "jsonp" && s->jsonpCallback.isEmpty())
					{
						respondError(s->req, 400, "Bad Request", "Bad Request");
						return;
					}

					s->sid = sid;
					s->lastPart = lastPart;
					sessionsById.insert(s->sid, s);
					s->pending = true;
					pendingSessions += s;
					emit q->sessionReady();
					return;
				}
			}

			respondError(s->req, 404, "Not Found", "Not Found");
		}
	}

	void handleSocket(Session *s)
	{
		if(s->path == "/websocket")
		{
			s->pending = true;
			pendingSessions += s;
			emit q->sessionReady();
			return;
		}
		else
		{
			QList<QByteArray> parts = s->path.mid(1).split('/');
			if(parts.count() == 3)
			{
				QByteArray sid = parts[1];
				QByteArray lastPart = parts[2];

				s->sid = sid;
				s->lastPart = lastPart;
				s->pending = true;
				pendingSessions += s;
				emit q->sessionReady();
				return;
			}

			respondError(s->sock, 404, "Not Found", "Not Found");
		}
	}

	SockJsSession *takeNext()
	{
		Session *s = 0;

		while(!s)
		{
			if(pendingSessions.isEmpty())
				return 0;

			s = pendingSessions.takeFirst();
			s->pending = false;
			if(!s->req && !s->sock)
			{
				// this means the object was a zombie. clean up and take next
				removeSession(s);
				s = 0;
				continue;
			}
		}

		s->ext = new SockJsSession;

		if(s->req)
		{
			assert(!s->sid.isEmpty());
			assert(!s->lastPart.isEmpty());

			s->ext->setupServer(q, s->req, s->jsonpCallback, s->asUri, s->sid, s->lastPart, s->reqBody.toByteArray(), s->route);

			s->req->disconnect(this);
			sessionsByRequest.remove(s->req);
			s->req = 0;
		}
		else // s->sock
		{
			if(!s->sid.isEmpty())
			{
				assert(!s->lastPart.isEmpty());
				s->ext->setupServer(q, s->sock, s->asUri, s->sid, s->lastPart, s->route);
			}
			else
				s->ext->setupServer(q, s->sock, s->asUri, s->route);

			s->sock->disconnect(this);
			sessionsBySocket.remove(s->sock);
			s->sock = 0;
		}

		sessionsByExt.insert(s->ext, s);
		s->ext->startServer();
		return s->ext;
	}

private slots:
	void req_readyRead()
	{
		ZhttpRequest *req = (ZhttpRequest *)sender();

		// for a request to have been discardable, we must have read the
		//   entire input already and handed to the session
		assert(!discardedRequests.contains(req));

		Session *s = sessionsByRequest.value(req);
		assert(s);

		processRequestInput(s);
	}

	void req_bytesWritten(int count)
	{
		Q_UNUSED(count);

		ZhttpRequest *req = (ZhttpRequest *)sender();

		if(discardedRequests.contains(req))
		{
			if(req->isFinished())
			{
				discardedRequests.remove(req);
				delete req;
			}

			return;
		}

		Session *s = sessionsByRequest.value(req);
		assert(s);

		if(req->isFinished())
		{
			assert(!s->pending);
			removeSession(s);
		}
	}

	void req_error()
	{
		ZhttpRequest *req = (ZhttpRequest *)sender();

		if(discardedRequests.contains(req))
		{
			discardedRequests.remove(req);
			delete req;
			return;
		}

		Session *s = sessionsByRequest.value(req);
		assert(s);

		if(s->pending)
			s->req = 0;
		else
			removeSession(s);
	}

	void sock_closed()
	{
		ZWebSocket *sock = (ZWebSocket *)sender();
		Session *s = sessionsBySocket.value(sock);
		assert(s);

		if(s->pending)
			s->sock = 0;
		else
			removeSession(s);
	}

	void sock_error()
	{
		ZWebSocket *sock = (ZWebSocket *)sender();
		Session *s = sessionsBySocket.value(sock);
		assert(s);

		if(s->pending)
			s->sock = 0;
		else
			removeSession(s);
	}

	void timer_timeout()
	{
		QTimer *timer = (QTimer *)sender();
		Session *s = sessionsByTimer.value(timer);
		assert(s);

		assert(!s->pending);
		removeSession(s);
	}
};

SockJsManager::SockJsManager(const QString &sockJsUrl, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, sockJsUrl);
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
	return d->takeNext();
}

void SockJsManager::unlink(SockJsSession *sess)
{
	d->unlink(sess);
}

void SockJsManager::setLinger(SockJsSession *ext, const QVariant &closeValue)
{
	d->setLinger(ext, closeValue);
}

void SockJsManager::respondOk(ZhttpRequest *req, const QVariant &data, const QByteArray &prefix, const QByteArray &jsonpCallback)
{
	d->respondOk(req, data, prefix, jsonpCallback);
}

void SockJsManager::respondOk(ZhttpRequest *req, const QString &str, const QByteArray &jsonpCallback)
{
	d->respondOk(req, str, jsonpCallback);
}

void SockJsManager::respondError(ZhttpRequest *req, int code, const QByteArray &reason, const QString &message, bool discard)
{
	d->respondError(req, code, reason, message, discard);
}

void SockJsManager::respond(ZhttpRequest *req, int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
{
	d->respond(req, code, reason, headers, body);
}

#include "sockjsmanager.moc"
