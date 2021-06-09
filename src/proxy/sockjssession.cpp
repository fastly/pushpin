/*
 * Copyright (C) 2015-2021 Fanout, Inc.
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

#include "sockjssession.h"

#include <assert.h>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "log.h"
#include "bufferlist.h"
#include "packet/httprequestdata.h"
#include "zhttprequest.h"
#include "zwebsocket.h"
#include "sockjsmanager.h"

#define BUFFER_SIZE 200000
#define KEEPALIVE_TIMEOUT 25
#define UNCONNECTED_TIMEOUT 5

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

	class RequestItem
	{
	public:
		enum Type
		{
			Background,
			Connect,
			Accept,
			Reject,
			Send, // data from client
			Receive, // data to client
			ReceiveClose // close to client
		};

		ZhttpRequest *req;
		QByteArray jsonpCallback;
		Type type;
		bool responded;

		QList<Frame> sendFrames;
		int sendBytes;
		int receiveFrames;
		int receiveBytes;

		RequestItem(ZhttpRequest *_req, const QByteArray &_jsonpCallback, Type _type, bool _responded = false) :
			req(_req),
			jsonpCallback(_jsonpCallback),
			type(_type),
			responded(_responded),
			sendBytes(0),
			receiveFrames(0),
			receiveBytes(0)
		{
		}

		~RequestItem()
		{
			delete req;
		}
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
	ZWebSocket *sock;
	bool passThrough;
	QList<Frame> inWrappedFrames;
	QList<Frame> inFrames;
	QList<Frame> outFrames;
	int inBytes;
	int pendingWrittenFrames;
	int pendingWrittenBytes;
	QList<WriteItem> pendingWrites;
	QHash<ZhttpRequest*, RequestItem*> requests;
	QTimer *keepAliveTimer;
	int closeCode;
	QString closeReason;
	bool closeSent;
	bool peerClosed;
	int peerCloseCode;
	QString peerCloseReason;
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
		inBytes(0),
		pendingWrittenFrames(0),
		pendingWrittenBytes(0),
		closeCode(-1),
		closeSent(false),
		peerClosed(false),
		peerCloseCode(-1),
		updating(false)
	{
		keepAliveTimer = new QTimer(this);
		connect(keepAliveTimer, &QTimer::timeout, this, &Private::keepAliveTimer_timeout);
	}

	~Private()
	{
		keepAliveTimer->disconnect(this);
		keepAliveTimer->setParent(0);
		keepAliveTimer->deleteLater();

		cleanup();
	}

	void removeRequestItem(RequestItem *ri)
	{
		requests.remove(ri->req);
		delete ri;
	}

	RequestItem *findFirstSendRequest()
	{
		QHashIterator<ZhttpRequest*, RequestItem*> it(requests);
		while(it.hasNext())
		{
			it.next();

			RequestItem *ri = it.value();
			if(ri->type == RequestItem::Send)
				return ri;
		}

		return 0;
	}

	void cleanup()
	{
		keepAliveTimer->stop();

		if(req)
		{
			RequestItem *ri = requests.value(req);
			assert(ri);

			// detach req from RequestItem
			requests.remove(req);
			ri->req = 0;
			delete ri;

			// discard=true to let manager take over
			manager->respondError(req, 410, "Gone", "Session terminated", true);

			req = 0;
		}

		QHashIterator<ZhttpRequest*, RequestItem*> it(requests);
		while(it.hasNext())
		{
			it.next();
			delete it.value();
		}
		requests.clear();

		delete sock;
		sock = 0;

		if(manager)
		{
			manager->unlink(q);
			manager = 0;
		}
	}

	void setup()
	{
		if(mode == Http)
		{
			req = initialReq;
			initialReq = 0;
			QByteArray jsonpCallback = initialJsonpCallback;
			initialJsonpCallback.clear();

			// don't need these things
			initialLastPart.clear();
			initialBody.clear();

			requests.insert(req, new RequestItem(req, jsonpCallback, RequestItem::Connect));

			connect(req, &ZhttpRequest::bytesWritten, this, &Private::req_bytesWritten);
			connect(req, &ZhttpRequest::error, this, &Private::req_error);
		}
		else
		{
			connect(sock, &ZWebSocket::readyRead, this, &Private::sock_readyRead);
			connect(sock, &ZWebSocket::framesWritten, this, &Private::sock_framesWritten);
			connect(sock, &ZWebSocket::closed, this, &Private::sock_closed);
			connect(sock, &ZWebSocket::peerClosed, this, &Private::sock_peerClosed);
			connect(sock, &ZWebSocket::error, this, &Private::sock_error);
		}
	}

	void startServer()
	{
		state = Connecting;
	}

	void respondOk(ZhttpRequest *req, const QVariant &data, const QByteArray &prefix = QByteArray(), const QByteArray &jsonpCallback = QByteArray())
	{
		manager->respondOk(req, data, prefix, jsonpCallback);
	}

	void respondOk(ZhttpRequest *req, const QString &str, const QByteArray &jsonpCallback = QByteArray())
	{
		manager->respondOk(req, str, jsonpCallback);
	}

	void respondError(ZhttpRequest *req, int code, const QByteArray &reason, const QString &message)
	{
		manager->respondError(req, code, reason, message);
	}

	void respond(ZhttpRequest *req, int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		manager->respond(req, code, reason, headers, body);
	}

	void handleRequest(ZhttpRequest *_req, const QByteArray &jsonpCallback, const QByteArray &lastPart, const QByteArray &body)
	{
		connect(_req, &ZhttpRequest::bytesWritten, this, &Private::req_bytesWritten);
		connect(_req, &ZhttpRequest::error, this, &Private::req_error);

		if(lastPart == "xhr" || lastPart == "jsonp")
		{
			if(req)
			{
				QVariantList out;
				out += 2010;
				out += QString("Another connection still open");

				requests.insert(_req, new RequestItem(_req, jsonpCallback, RequestItem::Background, true));
				respondOk(_req, out, "c", jsonpCallback);
				return;
			}

			if(peerClosed)
			{
				QVariantList out;
				out += 3000;
				out += QString("Client already closed connection");

				requests.insert(_req, new RequestItem(_req, jsonpCallback, RequestItem::Background, true));
				respondOk(_req, out, "c", jsonpCallback);
				return;
			}

			req = _req;
			requests.insert(req, new RequestItem(req, jsonpCallback, RequestItem::Receive));
			keepAliveTimer->start(KEEPALIVE_TIMEOUT * 1000);

			tryWrite();
		}
		else if(lastPart == "xhr_send" || lastPart == "jsonp_send")
		{
			// only allow one outstanding send request at a time
			if(findFirstSendRequest())
			{
				requests.insert(_req, new RequestItem(_req, jsonpCallback, RequestItem::Background, true));
				respondError(_req, 400, "Bad Request", "Already sending");
				return;
			}

			QByteArray param;

			if(_req->requestMethod() == "POST")
			{
				if(lastPart == "xhr_send")
				{
					// assume json
					param = body;
				}
				else // jsonp_send
				{
					// assume form encoded
					foreach(const QByteArray &kv, body.split('&'))
					{
						int at = kv.indexOf('=');
						if(at == -1)
							continue;
						if(QUrl::fromPercentEncoding(kv.mid(0, at)) == "d")
						{
							param = QUrl::fromPercentEncoding(kv.mid(at + 1)).toUtf8();
							break;
						}
					}
				}
			}
			else // GET
			{
				QUrlQuery query(_req->requestUri());
				param = query.queryItemValue("d").toUtf8();
			}

			QJsonParseError error;
			QJsonDocument doc = QJsonDocument::fromJson(param, &error);
			if(error.error != QJsonParseError::NoError || !doc.isArray())
			{
				requests.insert(_req, new RequestItem(_req, jsonpCallback, RequestItem::Background, true));
				respondError(_req, 400, "Bad Request", "Payload expected");
				return;
			}

			QVariantList messages = doc.array().toVariantList();

			QList<Frame> frames;
			int bytes = 0;
			foreach(const QVariant &vmessage, messages)
			{
				if(vmessage.type() != QVariant::String)
				{
					requests.insert(_req, new RequestItem(_req, jsonpCallback, RequestItem::Background, true));
					respondError(_req, 400, "Bad Request", "Payload expected");
					return;
				}

				QByteArray data = vmessage.toString().toUtf8();
				if(data.size() > BUFFER_SIZE)
				{
					requests.insert(_req, new RequestItem(_req, jsonpCallback, RequestItem::Background, true));
					respondError(_req, 400, "Bad Request", "Message too large");
					return;
				}

				frames += Frame(Frame::Text, data, false);
				bytes += data.size();
			}

			if(frames.isEmpty())
			{
				requests.insert(_req, new RequestItem(_req, jsonpCallback, RequestItem::Background, true));
				respondOk(_req, QString("ok"), jsonpCallback);
				return;
			}

			RequestItem *ri = new RequestItem(_req, jsonpCallback, RequestItem::Send);
			requests.insert(_req, ri);
			ri->sendFrames = frames;
			ri->sendBytes = bytes;

			tryRead();
		}
	}

	void accept(const QByteArray &reason, const HttpHeaders &headers)
	{
		if(errored)
			return;

		if(mode == Http)
		{
			assert(req);
			RequestItem *ri = requests.value(req);
			assert(ri && !ri->responded);

			// note: reason/headers don't have meaning with sockjs http

			ri->type = RequestItem::Accept;
			ri->responded = true;
			respondOk(req, QVariant(), "o", ri->jsonpCallback);
		}
		else
		{
			assert(sock);

			sock->respondSuccess(reason, headers);

			state = Connected;

			if(mode == WebSocketFramed)
			{
				Frame f(Frame::Text, "o", false);
				pendingWrites += WriteItem(WriteItem::Transport);
				sock->writeFrame(f);

				keepAliveTimer->start(KEEPALIVE_TIMEOUT * 1000);
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
			RequestItem *ri = requests.value(req);
			assert(ri && !ri->responded);

			ri->type = RequestItem::Reject;
			ri->responded = true;
			respond(req, code, reason, headers, body);
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

		if(mode == WebSocketPassthrough)
		{
			sock->writeFrame(frame);
		}
		else
		{
			if(frame.type != Frame::Text && frame.type != Frame::Binary)
			{
				++pendingWrittenFrames;
				pendingWrittenBytes += frame.data.size();
				update();
				return;
			}

			if(mode == Http)
			{
				outFrames += frame;
				tryWrite();
			}
			else // WebSocketFramed
			{
				QVariantList messages;
				messages += QString::fromUtf8(frame.data);

				QByteArray arrayJson = QJsonDocument(QJsonArray::fromVariantList(messages)).toJson(QJsonDocument::Compact);
				Frame f(Frame::Text, "a" + arrayJson, false);

				pendingWrites += WriteItem(WriteItem::User, frame.data.size());
				sock->writeFrame(f);
			}
		}
	}

	Frame readFrame()
	{
		if(mode == Http || mode == WebSocketFramed)
		{
			Frame f = inFrames.takeFirst();
			inBytes -= f.data.size();
			update();
			return f;
		}
		else
		{
			return sock->readFrame();
		}
	}

	void close(int code, const QString &reason)
	{
		assert(state != Closing);

		state = Closing;
		closeCode = code;
		closeReason = reason;

		if(mode == Http)
		{
			if(peerClosed)
			{
				state = Idle;
				applyLinger();
				cleanup();
				QMetaObject::invokeMethod(q, "closed", Qt::QueuedConnection);
			}
			else
				tryWrite();
		}
		else
		{
			assert(sock);

			sock->close(closeCode, closeReason);
		}
	}

	void tryWrite()
	{
		if(!req || closeSent)
			return;

		RequestItem *ri = requests.value(req);
		assert(ri);

		if(ri->responded)
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

			assert(first.type == Frame::Text || first.type == Frame::Binary);

			QByteArray data = bufs.toByteArray();

			pendingWrites += WriteItem(WriteItem::User, data.size());
			messages += QString::fromUtf8(data);
		}

		ri->receiveFrames = frames;
		ri->receiveBytes = bytes;

		if(!messages.isEmpty())
		{
			ri->responded = true;
			respondOk(req, messages, "a", ri->jsonpCallback);
			keepAliveTimer->stop();
		}
		else if(state == Closing)
		{
			closeSent = true;
			QVariant closeValue = applyLinger();

			ri->type = RequestItem::ReceiveClose;
			ri->responded = true;
			respondOk(req, closeValue, "c", ri->jsonpCallback);
		}
	}

	bool tryRead()
	{
		QPointer<QObject> self = this;

		if(mode == Http)
		{
			QList<RequestItem*> sendRequests;
			QHashIterator<ZhttpRequest*, RequestItem*> it(requests);
			while(it.hasNext())
			{
				it.next();
				RequestItem *ri = it.value();

				if(ri->type == RequestItem::Send && !ri->responded)
					sendRequests += ri;
			}

			bool emitReadyRead = false;

			foreach(RequestItem *ri, sendRequests)
			{
				assert(!ri->sendFrames.isEmpty());

				if(inBytes + ri->sendFrames.first().data.size() > BUFFER_SIZE)
					break;

				Frame f = ri->sendFrames.takeFirst();
				ri->sendBytes -= f.data.size();

				if(ri->sendFrames.isEmpty())
				{
					assert(ri->sendBytes == 0);

					ri->responded = true;
					respondOk(ri->req, QString("ok"), ri->jsonpCallback);
				}

				inFrames += f;
				inBytes += f.data.size();

				emitReadyRead = true;
			}

			if(emitReadyRead)
			{
				emit q->readyRead();
				if(!self)
					return false;
			}
		}
		else if(mode == WebSocketFramed)
		{
			bool error = false;
			bool emitReadyRead = false;

			while(inBytes < BUFFER_SIZE)
			{
				int end = 0;
				for(; end < inWrappedFrames.count(); ++end)
				{
					if(!inWrappedFrames[end].more)
						break;
				}
				if(end >= inWrappedFrames.count())
				{
					if(sock->framesAvailable() == 0)
						break;

					Frame f = sock->readFrame();

					// allow a larger temporary read size due to wrapping
					if(f.data.size() > BUFFER_SIZE * 2)
					{
						error = true;
						break;
					}

					inWrappedFrames += f;
					continue;
				}

				int size = 0;
				for(int n = 0; n <= end; ++n)
					size += inWrappedFrames[n].data.size();

				// allow a larger temporary read size due to wrapping
				if(size > BUFFER_SIZE * 2)
				{
					error = true;
					break;
				}

				Frame first = inWrappedFrames[0];

				BufferList bufs;
				for(int n = 0; n <= end; ++n)
				{
					Frame f = inWrappedFrames.takeFirst();
					bufs += f.data;
				}

				if(first.type != Frame::Text && first.type != Frame::Binary)
					continue;

				QByteArray data = bufs.toByteArray();

				QJsonParseError e;
				QJsonDocument doc = QJsonDocument::fromJson(data, &e);
				if(e.error != QJsonParseError::NoError || !doc.isArray())
				{
					error = true;
					break;
				}

				QVariantList messages = doc.array().toVariantList();

				QList<Frame> frames;
				int bytes = 0;
				foreach(const QVariant &vmessage, messages)
				{
					if(vmessage.type() != QVariant::String)
					{
						error = true;
						break;
					}

					data = vmessage.toString().toUtf8();
					if(data.size() > BUFFER_SIZE)
					{
						error = true;
						break;
					}

					frames += Frame(Frame::Text, data, false);
					bytes += data.size();
				}

				if(error)
					break;

				// note: inBytes may exceed BUFFER_SIZE at this point, but
				//   it shouldn't be by more than double

				inFrames += frames;
				inBytes += bytes;
				emitReadyRead = true;
			}

			if(error)
			{
				state = Idle;
				cleanup();
				emit q->error();

				// stop signals
				return false;
			}

			if(emitReadyRead)
			{
				emit q->readyRead();
				if(!self)
					return false;
			}
		}

		return true;
	}

	void update()
	{
		if(!updating)
		{
			updating = true;
			QMetaObject::invokeMethod(this, "doUpdate", Qt::QueuedConnection);
		}
	}

	void handleWritten(int count, int contentBytes)
	{
		if(mode == Http || mode == WebSocketFramed)
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

			count += pendingWrittenFrames;
			contentBytes += pendingWrittenBytes;
			pendingWrittenFrames = 0;
			pendingWrittenBytes = 0;
		}

		emit q->framesWritten(count, contentBytes);
	}

	QVariant applyLinger()
	{
		QVariantList closeValue;

		if(closeCode != -1)
			closeValue += closeCode;
		else
			closeValue += 0;

		if(closeCode != -1 && !closeReason.isEmpty())
			closeValue += closeReason;
		else
			closeValue += QString("Connection closed");

		manager->setLinger(q, closeValue);
		return closeValue;
	}

private slots:
	void req_bytesWritten(int count)
	{
		Q_UNUSED(count);

		ZhttpRequest *_req = (ZhttpRequest *)sender();
		RequestItem *ri = requests.value(_req);
		assert(ri);

		if(!_req->isFinished())
			return;

		if(ri->type == RequestItem::Accept)
		{
			assert(_req == req);
			state = Connected;
			req = 0;
			removeRequestItem(ri);

			keepAliveTimer->start(UNCONNECTED_TIMEOUT * 1000);
		}
		else
		{
			if(_req == req)
			{
				req = 0;

				if(ri->type == RequestItem::Reject)
				{
					state = Idle;
					removeRequestItem(ri);
					cleanup();
					emit q->closed();
					return;
				}
				else if(ri->type == RequestItem::Receive)
				{
					int count = ri->receiveFrames;
					int contentBytes = ri->receiveBytes;
					removeRequestItem(ri);
					keepAliveTimer->start(UNCONNECTED_TIMEOUT * 1000);
					handleWritten(count, contentBytes);
					return;
				}
				else if(ri->type == RequestItem::ReceiveClose)
				{
					state = Idle;
					removeRequestItem(ri);
					cleanup();
					emit q->closed();
					return;
				}
			}

			removeRequestItem(ri);
		}
	}

	void req_error()
	{
		ZhttpRequest *_req = (ZhttpRequest *)sender();
		RequestItem *ri = requests.value(_req);
		assert(ri);

		if(ri->type == RequestItem::Connect ||
			ri->type == RequestItem::Accept ||
			ri->type == RequestItem::Reject ||
			ri->type == RequestItem::Receive ||
			ri->type == RequestItem::ReceiveClose)
		{
			assert(_req == req);

			// disconnect while long-polling means close, not error
			bool close = false;
			if(ri->type == RequestItem::Receive && !ri->responded)
				close = (_req->errorCondition() == ZhttpRequest::ErrorDisconnected);

			req = 0;
			removeRequestItem(ri);

			if(close && !peerClosed)
			{
				peerClosed = true;
				emit q->peerClosed();
				return;
			}

			state = Idle;
			cleanup();

			if(close)
				emit q->closed();
			else
				emit q->error();
		}
		else
		{
			removeRequestItem(ri);
		}
	}

	void sock_readyRead()
	{
		if(mode == WebSocketFramed)
		{
			tryRead();
		}
		else // WebSocketPassthrough
		{
			emit q->readyRead();
		}
	}

	void sock_framesWritten(int count, int contentBytes)
	{
		handleWritten(count, contentBytes);
	}

	void sock_peerClosed()
	{
		peerCloseCode = sock->peerCloseCode();
		peerCloseReason = sock->peerCloseReason();
		emit q->peerClosed();
	}

	void sock_closed()
	{
		peerCloseCode = sock->peerCloseCode();
		peerCloseReason = sock->peerCloseReason();
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

		if(mode == Http || mode == WebSocketFramed)
		{
			if(!tryRead())
				return;

			if(pendingWrittenFrames > 0)
			{
				int count = pendingWrittenFrames;
				int contentBytes = pendingWrittenBytes;
				pendingWrittenFrames = 0;
				pendingWrittenBytes = 0;

				emit q->framesWritten(count, contentBytes);
			}
		}
	}

	void keepAliveTimer_timeout()
	{
		assert(mode != WebSocketPassthrough);

		if(mode == Http)
		{
			if(req)
			{
				RequestItem *ri = requests.value(req);
				assert(ri && !ri->responded);

				ri->responded = true;
				respondOk(req, QVariant(), "h", ri->jsonpCallback);
			}
			else
			{
				// timeout while unconnected
				state = Idle;
				cleanup();
				emit q->error();
			}
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

void SockJsSession::setTrustConnectHost(bool on)
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
	if(d->mode == Private::Http || d->mode == Private::WebSocketFramed)
	{
		return d->inFrames.count();
	}
	else
	{
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

QString SockJsSession::peerCloseReason() const
{
	return d->peerCloseReason;
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

void SockJsSession::close(int code, const QString &reason)
{
	d->close(code, reason);
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
	d->peerAddress = sock->peerAddress();
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
	d->peerAddress = sock->peerAddress();
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
