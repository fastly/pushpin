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

#include "sockjssession.h"

#include <assert.h>
#include <QTimer>
#include <qjson/parser.h>
#include <qjson/serializer.h>
#include "log.h"
#include "bufferlist.h"
#include "packet/httprequestdata.h"
#include "zhttprequest.h"
#include "zwebsocket.h"
#include "sockjsmanager.h"

#define BUFFER_SIZE 200000
#define KEEPALIVE_TIMEOUT 25

class SockJsSession::Private : public QObject
{
	Q_OBJECT

public:
	enum Mode
	{
		Http,
		WebSocketFramed,
		WebSocketPassthrough
	};

	class WriteItem
	{
	public:
		enum Type
		{
			Transport,
			User
		};

		Type type;
		int size;

		WriteItem(Type _type, int _size = 0) :
			type(_type),
			size(_size)
		{
		}
	};

	SockJsSession *q;
	SockJsManager *manager;
	Mode mode;
	QByteArray sid;
	DomainMap::Entry route;
	HttpRequestData requestData;
	QHostAddress peerAddress;
	State state;
	bool errored;
	ErrorCondition errorCondition;
	ZhttpRequest *initialReq;
	QByteArray initialJsonpCallback;
	QByteArray initialLastPart;
	QByteArray initialBody;
	ZhttpRequest *req;
	QByteArray jsonpCallback;
	ZWebSocket *sock;
	bool passThrough;
	QList<Frame> inFrames;
	QList<Frame> outFrames;
	int outPendingBytes;
	int pendingWrittenFrames;
	int pendingWrittenBytes;
	QList<WriteItem> pendingWrites;
	QTimer *keepAliveTimer;
	int closeCode;
	int peerCloseCode;
	bool updating;

	Private(SockJsSession *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		mode((Mode)-1),
		state(WebSocket::Idle),
		errored(false),
		errorCondition(WebSocket::ErrorGeneric),
		initialReq(0),
		req(0),
		sock(0),
		outPendingBytes(0),
		pendingWrittenFrames(0),
		pendingWrittenBytes(0),
		closeCode(-1),
		peerCloseCode(-1),
		updating(false)
	{
		keepAliveTimer = new QTimer(this);
		connect(keepAliveTimer, SIGNAL(timeout()), SLOT(keepAliveTimer_timeout()));
	}

	~Private()
	{
		keepAliveTimer->disconnect(this);
		keepAliveTimer->setParent(0);
		keepAliveTimer->deleteLater();

		if(manager)
			manager->unlink(q);
	}

	void cleanup()
	{
		keepAliveTimer->stop();
	}

	void setup()
	{
		if(mode == Http)
		{
			req = initialReq;
			initialReq = 0;
			jsonpCallback = initialJsonpCallback;
			initialJsonpCallback.clear();

			// don't need these things
			initialLastPart.clear();
			initialBody.clear();

			connect(req, SIGNAL(error()), SLOT(req_error()));
		}
		else
		{
			connect(sock, SIGNAL(connected()), SLOT(sock_connected()));
			connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
			connect(sock, SIGNAL(framesWritten(int, int)), SLOT(sock_framesWritten(int, int)));
			connect(sock, SIGNAL(closed()), SLOT(sock_closed()));
			connect(sock, SIGNAL(peerClosed()), SLOT(sock_peerClosed()));
			connect(sock, SIGNAL(error()), SLOT(sock_error()));
		}
	}

	void startServer()
	{
		state = Connecting;

		if(mode != Http)
			keepAliveTimer->start(KEEPALIVE_TIMEOUT * 1000);
	}

	void respondOk(ZhttpRequest *req, const QVariant &data, const QByteArray &prefix = QByteArray(), const QByteArray &jsonpCallback = QByteArray())
	{
		manager->respondOk(req, data, prefix, jsonpCallback);
	}

	void respondError(ZhttpRequest *req, int code, const QByteArray &reason, const QString &message)
	{
		manager->respondError(req, code, reason, message);
	}

	void handleRequest(ZhttpRequest *_req, const QByteArray &jsonpCallback, const QByteArray &lastPart, const QByteArray &body)
	{
			/*if(lastPart == "xhr" || lastPart == "jsonp")
			{
				if(req)
				{
					QVariantList out;
					out += 2010;
					out += QString("Another connection still open");
					respondOk(s->req, out, "c", s->jsonpCallback);

					[2010, \"Another connection still open\"]
					respondOk(_req, "c\n");
					return;
				}

				// TODO: monitor for errors, send timeout response
				keepAliveTimer->start(KEEPALIVE_TIMEOUT * 1000);
				req = _req;
				connect(req, SIGNAL(error()), SLOT(req_error()));
			}
			else if(lastPart == "xhr_send" || lastPart == "jsonp_send")
			{
				// TODO: jsonp handling

				QJson::Parser parser;
				bool ok;
				QVariant vmessages = parser.parse(body, &ok);
				if(!ok || vmessages.type() != QVariant::List)
				{
					respondError(_req, 400, "Bad Request", "Payload expected");
					return;
				}

				// TODO: flow control? don't respond unless we can accept

				QList<Frame> frames;
				foreach(const QVariant &vmessage, vmessages.toList())
				{
					if(vmessage.type() != QVariant::String)
					{
						respondError(_req, 400, "Bad Request", "Payload expected");
						return;
					}

					frames += Frame(Frame::Text, vmessage.toString().toUtf8(), false);
				}

				respondEmpty(_req);

				inFrames += frames;
				emit q->readyRead();
			}
		}*/
	}

	void accept(const QByteArray &reason, const HttpHeaders &headers)
	{
		if(errored)
			return;

		if(mode == Http)
		{
			assert(req);

			// note: reason/headers don't have meaning with sockjs http

			respondOk(req, "application/javascript", "o\n");
			req = 0;
			state = Connected;
		}
		else
		{
			assert(sock);

			sock->respondSuccess(reason, headers);

			if(mode == WebSocketFramed)
			{
				Frame f(Frame::Text, "o", false);
				pendingWrites += WriteItem(WriteItem::Transport);
				sock->writeFrame(f);
			}
		}
	}

	void reject(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		if(errored)
			return;

		if(mode == Http)
		{
			assert(req);

			//respond(req, code, reason, headers, body);
			req = 0;
			cleanup();
			QMetaObject::invokeMethod(this, "doClosed", Qt::QueuedConnection);
		}
		else
		{
			assert(sock);

			sock->respondError(code, reason, headers, body);
		}
	}

	void writeFrame(const Frame &frame)
	{
		assert(state != Closing);

		if(mode == Http)
		{
			outFrames += frame;
			trySend();
		}
		else
		{
			assert(sock);

			if(mode == WebSocketFramed)
			{
				if(frame.type == Frame::Text || frame.type == Frame::Binary)
				{
					QVariantList messages;
					messages += QString::fromUtf8(frame.data);

					QJson::Serializer serializer;
					QByteArray arrayJson = serializer.serialize(messages);
					Frame f(Frame::Text, "a" + arrayJson, false);

					pendingWrites += WriteItem(WriteItem::User, arrayJson.size());
					sock->writeFrame(f);
				}
				else
				{
					++pendingWrittenFrames;
					pendingWrittenBytes += frame.data.size();
					update();
				}
			}
			else // WebSocketPassthrough
			{
				sock->writeFrame(frame);
			}
		}
	}

	Frame readFrame()
	{
		if(mode == Http)
		{
			return inFrames.takeFirst();
		}
		else
		{
			assert(sock);

			return sock->readFrame();
		}
	}

	void close(int code)
	{
		assert(state != Closing);

		state = Closing;
		closeCode = code;

		if(mode == Http)
		{
			// TODO
		}
		else
		{
			assert(sock);

			sock->close(closeCode);
		}
	}

	void trySend()
	{
		if(!req)
			return;

		QVariantList messages;

		int frames = 0;
		int bytes = 0;
		while(!outFrames.isEmpty())
		{
			// find end
			int end = 0;
			for(; end < outFrames.count(); ++end)
			{
				if(!outFrames[end].more)
					break;
			}
			if(end >= outFrames.count())
				break;

			Frame first = outFrames[0];

			BufferList bufs;
			for(int n = 0; n <= end; ++n)
			{
				Frame f = outFrames.takeFirst();
				++frames;
				bytes += f.data.size();
				bufs += f.data;
			}

			if(first.type == Frame::Text || first.type == Frame::Binary)
				messages += QString::fromUtf8(bufs.toByteArray());
		}

		if(!messages.isEmpty())
		{
			QJson::Serializer serializer;
			QByteArray arrayJson = serializer.serialize(messages);
			respondOk(req, "application/javascript", "a" + arrayJson + "\n");
			req = 0;
			keepAliveTimer->stop();
			QMetaObject::invokeMethod(q, "bytesWritten", Qt::QueuedConnection, Q_ARG(int, frames), Q_ARG(int, bytes));
		}
		else if(state == Closing)
		{
			QVariantList closeArray;
			closeArray += closeCode;
			closeArray += QString("Connection closed");
			QJson::Serializer serializer;
			QByteArray arrayJson = serializer.serialize(closeArray);
			respondOk(req, "application/javascript", "c" + arrayJson + "\n");
			req = 0;
			state = Idle;
			// TODO: manager needs to linger this session
			cleanup();
			QMetaObject::invokeMethod(this, "doClosed", Qt::QueuedConnection);
		}
	}

	void update()
	{
		if(!updating)
		{
			updating = true;
			QMetaObject::invokeMethod(this, "doUpdate", Qt::QueuedConnection);
		}
	}

private slots:
	void req_error()
	{
		// FIXME: disconnect is clean close, timeout is not
		delete req;
		req = 0;
		state = Idle;
		cleanup();
		emit q->closed();
	}

	void sock_connected()
	{
		state = Connected;

		emit q->connected();
	}

	void sock_readyRead()
	{
		emit q->readyRead();
	}

	void sock_framesWritten(int count, int contentBytes)
	{
		if(mode == WebSocketFramed)
		{
			int newCount = 0;
			int newContentBytes = 0;
			for(int n = 0; n < count; ++n)
			{
				WriteItem i = pendingWrites.takeFirst();
				if(i.type == WriteItem::User)
				{
					++newCount;
					newContentBytes += i.size;
				}
			}

			count = newCount;
			contentBytes = newContentBytes;
		}

		count += pendingWrittenFrames;
		contentBytes += pendingWrittenBytes;
		pendingWrittenFrames = 0;
		pendingWrittenBytes = 0;

		emit q->framesWritten(count, contentBytes);
	}

	void sock_peerClosed()
	{
		peerCloseCode = sock->peerCloseCode();
		emit q->peerClosed();
	}

	void sock_closed()
	{
		peerCloseCode = sock->peerCloseCode();
		state = Idle;
		cleanup();
		emit q->closed();
	}

	void sock_error()
	{
		state = Idle;
		errorCondition = sock->errorCondition();
		cleanup();
		emit q->error();
	}

	void doUpdate()
	{
		updating = false;

		if(pendingWrittenFrames > 0)
		{
			int count = pendingWrittenFrames;
			int contentBytes = pendingWrittenBytes;
			pendingWrittenFrames = 0;
			pendingWrittenBytes = 0;

			emit q->framesWritten(count, contentBytes);
		}
	}

	void doClosed()
	{
		emit q->closed();
	}

	void doError()
	{
		emit q->error();
	}

	void keepAliveTimer_timeout()
	{
		assert(mode != WebSocketPassthrough);

		if(mode == Http)
		{
			assert(req);

			respondOk(req, "application/javascript", "h\n");
			req = 0;
		}
		else
		{
			assert(sock);

			Frame f(Frame::Text, "h", false);
			pendingWrites += WriteItem(WriteItem::Transport);
			sock->writeFrame(f);
		}
	}
};

SockJsSession::SockJsSession(QObject *parent) :
	WebSocket(parent)
{
	d = new Private(this);
}

SockJsSession::~SockJsSession()
{
	delete d;
}

QByteArray SockJsSession::sid() const
{
	return d->sid;
}

DomainMap::Entry SockJsSession::route() const
{
	return d->route;
}

QHostAddress SockJsSession::peerAddress() const
{
	return d->peerAddress;
}

void SockJsSession::setConnectHost(const QString &host)
{
	Q_UNUSED(host);

	// this class is server only
	assert(0);
}

void SockJsSession::setConnectPort(int port)
{
	Q_UNUSED(port);

	// this class is server only
	assert(0);
}

void SockJsSession::setIgnorePolicies(bool on)
{
	Q_UNUSED(on);

	// this class is server only
	assert(0);
}

void SockJsSession::setIgnoreTlsErrors(bool on)
{
	Q_UNUSED(on);

	// this class is server only
	assert(0);
}

void SockJsSession::start(const QUrl &uri, const HttpHeaders &headers)
{
	Q_UNUSED(uri);
	Q_UNUSED(headers);

	// this class is server only
	assert(0);
}

void SockJsSession::respondSuccess(const QByteArray &reason, const HttpHeaders &headers)
{
	d->accept(reason, headers);
}

void SockJsSession::respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
{
	d->reject(code, reason, headers, body);
}

WebSocket::State SockJsSession::state() const
{
	return d->state;
}

QUrl SockJsSession::requestUri() const
{
	return d->requestData.uri;
}

HttpHeaders SockJsSession::requestHeaders() const
{
	return d->requestData.headers;
}

int SockJsSession::responseCode() const
{
	// this class is server only
	assert(0);
	return -1;
}

QByteArray SockJsSession::responseReason() const
{
	// this class is server only
	assert(0);
	return QByteArray();
}

HttpHeaders SockJsSession::responseHeaders() const
{
	// this class is server only
	assert(0);
	return HttpHeaders();
}

QByteArray SockJsSession::responseBody() const
{
	// this class is server only
	assert(0);
	return QByteArray();
}

int SockJsSession::framesAvailable() const
{
	if(d->mode == Private::Http)
	{
		return d->inFrames.count();
	}
	else
	{
		assert(d->sock);
		return d->sock->framesAvailable();
	}
}

bool SockJsSession::canWrite() const
{
	return (writeBytesAvailable() > 0);
}

int SockJsSession::writeBytesAvailable() const
{
	if(d->mode == Private::Http)
	{
		int avail = BUFFER_SIZE;
		foreach(const Frame &f, d->outFrames)
		{
			if(f.data.size() >= avail)
				return 0;

			avail -= f.data.size();
		}

		return avail;
	}
	else
	{
		assert(d->sock);
		return d->sock->writeBytesAvailable();
	}
}

int SockJsSession::peerCloseCode() const
{
	return d->peerCloseCode;
}

WebSocket::ErrorCondition SockJsSession::errorCondition() const
{
	return d->errorCondition;
}

void SockJsSession::writeFrame(const Frame &frame)
{
	d->writeFrame(frame);
}

WebSocket::Frame SockJsSession::readFrame()
{
	return d->readFrame();
}

void SockJsSession::close(int code)
{
	d->close(code);
}

void SockJsSession::setupServer(SockJsManager *manager, ZhttpRequest *req, const QByteArray &jsonpCallback, const QUrl &asUri, const QByteArray &sid, const QByteArray &lastPart, const QByteArray &body, const DomainMap::Entry &route)
{
	d->manager = manager;
	d->mode = Private::Http;
	d->sid = sid;
	d->requestData.uri = asUri;
	d->requestData.headers = req->requestHeaders();
	// we're not forwarding the request content so ignore this
	d->requestData.headers.removeAll("Content-Length");
	d->peerAddress = req->peerAddress();
	d->route = route;
	d->initialReq = req;
	d->initialJsonpCallback = jsonpCallback;
	d->initialLastPart = lastPart;
	d->initialBody = body;

	d->setup();
}

void SockJsSession::setupServer(SockJsManager *manager, ZWebSocket *sock, const QUrl &asUri, const DomainMap::Entry &route)
{
	d->manager = manager;
	d->mode = Private::WebSocketPassthrough;
	d->requestData.uri = asUri;
	d->requestData.headers = sock->requestHeaders();
	d->route = route;
	d->sock = sock;

	d->setup();
}

void SockJsSession::setupServer(SockJsManager *manager, ZWebSocket *sock, const QUrl &asUri, const QByteArray &sid, const QByteArray &lastPart, const DomainMap::Entry &route)
{
	Q_UNUSED(lastPart);

	d->manager = manager;
	d->mode = Private::WebSocketFramed;
	d->sid = sid;
	d->requestData.uri = asUri;
	d->requestData.headers = sock->requestHeaders();
	d->route = route;
	d->sock = sock;

	d->setup();
}

void SockJsSession::startServer()
{
	d->startServer();
}

void SockJsSession::handleRequest(ZhttpRequest *req, const QByteArray &jsonpCallback, const QByteArray &lastPart, const QByteArray &body)
{
	d->handleRequest(req, jsonpCallback, lastPart, body);
}

#include "sockjssession.moc"
