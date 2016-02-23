/*
 * Copyright (C) 2014-2015 Fanout, Inc.
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

#include "websocketoverhttp.h"

#include <assert.h>
#include <QTimer>
#include <QPointer>
#include <QCoreApplication>
#include "log.h"
#include "bufferlist.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "zhttprequest.h"
#include "zhttpmanager.h"
#include "uuidutil.h"

#define BUFFER_SIZE 200000
#define RESPONSE_BODY_MAX 1000000
#define REJECT_BODY_MAX 100000

namespace {

class WsEvent
{
public:
	QByteArray type;
	QByteArray content;

	WsEvent()
	{
	}

	WsEvent(const QByteArray &_type, const QByteArray &_content = QByteArray()) :
		type(_type),
		content(_content)
	{
	}
};

}

class WebSocketOverHttp::DisconnectManager : public QObject
{
	Q_OBJECT

public:
	DisconnectManager(QObject *parent = 0) :
		QObject(parent)
	{
	}

	void addSocket(WebSocketOverHttp *sock)
	{
		sock->setParent(this);
		connect(sock, SIGNAL(disconnected()), SLOT(sock_disconnected()));
		connect(sock, SIGNAL(error()), SLOT(sock_error()));

		sock->sendDisconnect();
	}

private:
	void cleanupSocket(WebSocketOverHttp *sock)
	{
		delete sock;
	}

private slots:
	void sock_disconnected()
	{
		WebSocketOverHttp *sock = (WebSocketOverHttp *)sender();
		cleanupSocket(sock);
	}

	void sock_error()
	{
		WebSocketOverHttp *sock = (WebSocketOverHttp *)sender();
		cleanupSocket(sock);
	}
};

WebSocketOverHttp::DisconnectManager *WebSocketOverHttp::g_disconnectManager = 0;

static QList<WsEvent> decodeEvents(const QByteArray &in, bool *ok = 0)
{
	QList<WsEvent> out;
	if(ok)
		*ok = false;

	int start = 0;
	while(start < in.size())
	{
		int at = in.indexOf("\r\n", start);
		if(at == -1)
			return QList<WsEvent>();

		QByteArray typeLine = in.mid(start, at - start);
		start = at + 2;

		WsEvent e;
		at = typeLine.indexOf(' ');
		if(at != -1)
		{
			e.type = typeLine.mid(0, at);

			bool check;
			int clen = typeLine.mid(at + 1).toInt(&check, 16);
			if(!check)
				return QList<WsEvent>();

			e.content = in.mid(start, clen);
			start += clen + 2;
		}
		else
		{
			e.type = typeLine;
		}

		out += e;
	}

	if(ok)
		*ok = true;
	return out;
}

static QByteArray encodeEvents(const QList<WsEvent> &events)
{
	QByteArray out;

	foreach(const WsEvent &e, events)
	{
		if(!e.content.isNull())
		{
			out += e.type + ' ' + QByteArray::number(e.content.size(), 16) + "\r\n" + e.content + "\r\n";
		}
		else
		{
			out += e.type + "\r\n";
		}
	}

	return out;
}

class WebSocketOverHttp::Private : public QObject
{
	Q_OBJECT

public:
	WebSocketOverHttp *q;
	ZhttpManager *zhttpManager;
	QString connectHost;
	int connectPort;
	bool ignorePolicies;
	bool ignoreTlsErrors;
	State state;
	QByteArray cid;
	HttpRequestData requestData;
	HttpResponseData responseData;
	ErrorCondition errorCondition;
	ErrorCondition pendingErrorCondition;
	int keepAliveInterval;
	HttpHeaders meta;
	bool updating;
	ZhttpRequest *req;
	int reqFrames;
	int reqContentSize;
	bool reqClose;
	BufferList inBuf;
	QList<Frame> inFrames;
	QList<Frame> outFrames;
	int closeCode;
	bool closeSent;
	bool peerClosing;
	int peerCloseCode;
	bool disconnecting;
	QTimer *keepAliveTimer;

	Private(WebSocketOverHttp *_q) :
		QObject(_q),
		q(_q),
		connectPort(-1),
		ignorePolicies(false),
		ignoreTlsErrors(false),
		state(WebSocket::Idle),
		errorCondition(ErrorGeneric),
		pendingErrorCondition((ErrorCondition)-1),
		keepAliveInterval(-1),
		updating(false),
		req(0),
		reqFrames(0),
		reqContentSize(0),
		reqClose(false),
		closeCode(-1),
		closeSent(false),
		peerClosing(false),
		peerCloseCode(-1),
		disconnecting(false)
	{
		if(!g_disconnectManager)
			g_disconnectManager = new DisconnectManager(QCoreApplication::instance());

		keepAliveTimer = new QTimer(this);
		connect(keepAliveTimer, SIGNAL(timeout()), SLOT(keepAliveTimer_timeout()));
		keepAliveTimer->setSingleShot(true);
	}

	~Private()
	{
		keepAliveTimer->disconnect(this);
		keepAliveTimer->setParent(0);
		keepAliveTimer->deleteLater();
	}

	void cleanup()
	{
		keepAliveTimer->stop();

		updating = false;

		delete req;
		req = 0;

		state = Idle;
	}

	void start()
	{
		state = Connecting;

		if(cid.isEmpty())
			cid = UuidUtil::createUuid();

		// don't forward certain headers
		requestData.headers.removeAll("Upgrade");
		requestData.headers.removeAll("Accept");
		requestData.headers.removeAll("Connection-Id");

		// don't forward headers starting with Meta-*
		for(int n = 0; n < requestData.headers.count(); ++n)
		{
			const HttpHeader &h = requestData.headers[n];
			if(qstrnicmp(h.first.data(), "Meta-", 5) == 0)
			{
				requestData.headers.removeAt(n);
				--n; // adjust position
			}
		}

		if(requestData.uri.scheme() == "wss")
			requestData.uri.setScheme("https");
		else
			requestData.uri.setScheme("http");

		update();
	}

	void writeFrame(const Frame &frame)
	{
		assert(state != Closing);

		outFrames += frame;

		if(needUpdate())
			update();
	}

	Frame readFrame()
	{
		return inFrames.takeFirst();
	}

	void close(int code)
	{
		assert(state != Closing);

		state = Closing;
		closeCode = code;

		update();
	}

	int writeBytesAvailable() const
	{
		int avail = BUFFER_SIZE;
		foreach(const Frame &f, outFrames)
		{
			if(f.data.size() >= avail)
				return 0;

			avail -= f.data.size();
		}

		return avail;
	}

	void sendDisconnect()
	{
		disconnecting = true;

		update();
	}

private:
	bool canReceive() const
	{
		int avail = 0;
		foreach(const Frame &f, inFrames)
		{
			avail += f.data.size();
			if(avail >= BUFFER_SIZE)
				return false;
		}

		return true;
	}

	bool canSendCompleteMessage() const
	{
		foreach(const Frame &f, outFrames)
		{
			if(!f.more)
				return true;
		}

		return false;
	}

	bool needUpdate() const
	{
		// always send this right away
		if(disconnecting)
			return true;

		bool cscm = canSendCompleteMessage();

		if(!cscm && writeBytesAvailable() == 0)
		{
			// write buffer maxed with incomplete message. this is
			//   unrecoverable. update to throw error right away.
			return true;
		}

		// if we can't fit a response then don't update yet
		if(!canReceive())
			return false;

		// have message to send or close?
		if(cscm || (outFrames.isEmpty() && state == Closing && !closeSent))
			return true;

		return false;
	}

	void queueError(ErrorCondition e)
	{
		if((int)pendingErrorCondition == -1)
		{
			pendingErrorCondition = e;
			QMetaObject::invokeMethod(this, "doError", Qt::QueuedConnection);
		}
	}

	void update()
	{
		// only one request allowed at a time
		if(updating)
			return;

		updating = true;

		keepAliveTimer->stop();

		// if we can't send yet but also have no room for writes, then fail
		if(!canSendCompleteMessage() && writeBytesAvailable() == 0)
		{
			updating = false;
			queueError(ErrorGeneric);
			return;
		}

		req = zhttpManager->createRequest();
		req->setParent(this);
		connect(req, SIGNAL(readyRead()), SLOT(req_readyRead()));
		connect(req, SIGNAL(bytesWritten(int)), SLOT(req_bytesWritten(int)));
		connect(req, SIGNAL(error()), SLOT(req_error()));

		if(!connectHost.isEmpty())
			req->setConnectHost(connectHost);
		if(connectPort != -1)
			req->setConnectPort(connectPort);
		req->setIgnorePolicies(ignorePolicies);
		req->setIgnoreTlsErrors(ignoreTlsErrors);

		HttpHeaders headers = requestData.headers;

		headers += HttpHeader("Accept", "application/websocket-events");
		headers += HttpHeader("Connection-Id", cid);
		headers += HttpHeader("Content-Type", "application/websocket-events");

		foreach(const HttpHeader &h, meta)
			headers += HttpHeader("Meta-" + h.first, h.second);

		reqFrames = 0;
		reqContentSize = 0;
		reqClose = false;

		QList<WsEvent> events;

		if(state == Connecting)
		{
			events += WsEvent("OPEN");
		}
		else if(disconnecting)
		{
			events += WsEvent("DISCONNECT");
		}
		else
		{
			while(!outFrames.isEmpty() && reqContentSize < BUFFER_SIZE)
			{
				// make sure the next message is fully readable
				int takeCount = -1;
				for(int n = 0; n < outFrames.count(); ++n)
				{
					if(!outFrames[n].more)
					{
						takeCount = n + 1;
						break;
					}
				}
				if(takeCount < 1)
					break;

				Frame::Type ftype;
				BufferList content;

				for(int n = 0; n < takeCount; ++n)
				{
					Frame f = outFrames.takeFirst();

					if((n == 0 && f.type == Frame::Continuation) || (n > 0 && f.type != Frame::Continuation))
					{
						updating = false;
						queueError(ErrorGeneric);
						return;
					}

					if(n == 0)
					{
						assert(f.type != Frame::Continuation);
						ftype = f.type;
					}

					content += f.data;

					assert(n + 1 < takeCount || !f.more);
				}

				QByteArray data = content.toByteArray();

				// for compactness, we only include content on ping/pong if non-empty
				if(ftype == Frame::Text)
					events += WsEvent("TEXT", data);
				else if(ftype == Frame::Binary)
					events += WsEvent("BINARY", data);
				else if(ftype == Frame::Ping)
					events += WsEvent("PING", !data.isEmpty() ? data : QByteArray());
				else if(ftype == Frame::Pong)
					events += WsEvent("PONG", !data.isEmpty() ? data : QByteArray());

				reqFrames += takeCount;
				reqContentSize += content.size();
			}

			if(state == Closing)
			{
				// if there was a partial message left, throw it away
				if(!outFrames.isEmpty())
				{
					log_warning("woh: dropping partial message before close");
					outFrames.clear();
				}

				if(closeCode != -1)
				{
					QByteArray buf(2, 0);
					buf[0] = (closeCode >> 8) & 0xff;
					buf[1] = closeCode & 0xff;
					events += WsEvent("CLOSE", buf);
				}
				else
					events += WsEvent("CLOSE");

				reqClose = true;
			}
		}

		QByteArray body = encodeEvents(events);
		headers += HttpHeader("Content-Length", QByteArray::number(body.size()));

		req->start("POST", requestData.uri, headers);
		req->writeBody(body);
		req->endBody();
	}

private slots:
	void req_readyRead()
	{
		if(inBuf.size() + req->bytesAvailable() > RESPONSE_BODY_MAX)
		{
			updating = false;

			delete req;
			req = 0;

			state = Idle;
			emit q->error();
			return;
		}

		inBuf += req->readBody();

		if(!req->isFinished())
		{
			updating = false;
			return;
		}

		int responseCode = req->responseCode();
		QByteArray responseReason = req->responseReason();
		HttpHeaders responseHeaders = req->responseHeaders();
		QByteArray responseBody = inBuf.take();

		delete req;
		req = 0;

		if(state == Connecting)
		{
			// save the initial response
			responseData.code = responseCode;
			responseData.reason = responseReason;
			responseData.headers = responseHeaders;
		}

		QByteArray contentType = responseHeaders.get("Content-Type");

		if(responseCode != 200 || contentType != "application/websocket-events")
		{
			updating = false;

			if(state == Connecting)
			{
				errorCondition = ErrorRejected;
				responseData.body = responseBody.mid(0, REJECT_BODY_MAX);
			}
			else
				errorCondition = ErrorGeneric;

			state = Idle;
			emit q->error();
			return;
		}

		if(responseHeaders.contains("Keep-Alive-Interval"))
		{
			bool ok;
			int x = responseHeaders.get("Keep-Alive-Interval").toInt(&ok);
			if(ok && x > 0)
			{
				if(x < 20)
					x = 20;

				keepAliveInterval = x;
			}
			else
				keepAliveInterval = -1;
		}

		foreach(const HttpHeader &h, responseHeaders)
		{
			if(h.first.size() >= 10 && qstrnicmp(h.first.data(), "Set-Meta-", 9) == 0)
			{
				QByteArray name = h.first.mid(9);
				if(meta.contains(name))
					meta.removeAll(name);
				meta += HttpHeader(name, h.second);
			}
		}

		bool ok;
		QList<WsEvent> events = decodeEvents(responseBody, &ok);
		if(!ok)
		{
			updating = false;

			state = Idle;
			emit q->error();
			return;
		}

		if(state == Connecting)
		{
			// server must respond with events or enable keep alive
			if(events.isEmpty() && keepAliveInterval == -1)
			{
				updating = false;

				state = Idle;
				emit q->error();
				return;
			}

			// first event must be OPEN
			if(!events.isEmpty() && events.first().type != "OPEN")
			{
				updating = false;

				state = Idle;
				emit q->error();
				return;
			}

			// strip private headers from the initial response
			responseData.headers.removeAll("Content-Length");
			responseData.headers.removeAll("Content-Type");
			responseData.headers.removeAll("Keep-Alive-Interval");
			for(int n = 0; n < responseData.headers.count(); ++n)
			{
				const HttpHeader &h = responseData.headers[n];
				if(h.first.size() >= 10 && qstrnicmp(h.first.data(), "Set-Meta-", 9) == 0)
				{
					responseData.headers.removeAt(n);
					--n; // adjust position
				}
			}
		}

		if(disconnecting)
		{
			updating = false;
			disconnecting = false;

			state = Idle;
			emit q->disconnected();
			return;
		}

		QPointer<QObject> self = this;

		bool emitConnected = false;
		bool emitReadyRead = false;
		bool closed = false;
		bool disconnected = false;

		foreach(const WsEvent &e, events)
		{
			if(e.type == "OPEN")
			{
				if(state != Connecting)
				{
					disconnected = true;
					break;
				}

				state = Connected;
				emitConnected = true;
			}
			else if(e.type == "TEXT")
			{
				inFrames += Frame(Frame::Text, e.content, false);
				emitReadyRead = true;
			}
			else if(e.type == "BINARY")
			{
				inFrames += Frame(Frame::Binary, e.content, false);
				emitReadyRead = true;
			}
			else if(e.type == "PING")
			{
				inFrames += Frame(Frame::Ping, e.content, false);
				emitReadyRead = true;
			}
			else if(e.type == "PONG")
			{
				inFrames += Frame(Frame::Pong, e.content, false);
				emitReadyRead = true;
			}
			else if(e.type == "CLOSE")
			{
				peerClosing = true;
				if(e.content.size() == 2)
					peerCloseCode = ((quint16)e.content[0] << 8) + (quint16)e.content[1];

				closed = true;
				break;
			}
			else if(e.type == "DISCONNECT")
			{
				disconnected = true;
				break;
			}
		}

		if(emitConnected)
		{
			emit q->connected();
			if(!self)
				return;
		}

		if(emitReadyRead)
		{
			emit q->readyRead();
			if(!self)
				return;
		}

		if(reqFrames > 0)
		{
			emit q->framesWritten(reqFrames, reqContentSize);
			if(!self)
				return;
		}

		if(reqClose)
			closeSent = true;

		if(closed)
		{
			if(closeSent)
			{
				updating = false;

				state = Idle;
				emit q->closed();
				return;
			}
			else
			{
				emit q->peerClosed();
			}
		}
		else if(closeSent && keepAliveInterval == -1)
		{
			// if there are no keep alives, then the server has only one
			//   chance to respond to a close. if it doesn't, then
			//   consider the connection uncleanly disconnected.
			disconnected = true;
		}

		if(disconnected)
		{
			updating = false;

			state = Idle;
			emit q->error();
			return;
		}

		if(reqClose && peerClosing)
		{
			updating = false;

			state = Idle;
			emit q->closed();
			return;
		}

		updating = false;

		if(needUpdate())
			update();
		else if(keepAliveInterval != -1)
			keepAliveTimer->start(keepAliveInterval * 1000);
	}

	void req_bytesWritten(int count)
	{
		Q_UNUSED(count);

		// nothing to do here
	}

	void req_error()
	{
		delete req;
		req = 0;

		state = Idle;
		emit q->error();
	}

	void keepAliveTimer_timeout()
	{
		update();
	}

	void doError()
	{
		cleanup();
		errorCondition = pendingErrorCondition;
		pendingErrorCondition = (ErrorCondition)-1;
		emit q->error();
	}
};

WebSocketOverHttp::WebSocketOverHttp(ZhttpManager *zhttpManager, QObject *parent) :
	WebSocket(parent)
{
	d = new Private(this);
	d->zhttpManager = zhttpManager;
}

WebSocketOverHttp::WebSocketOverHttp(QObject *parent) :
	WebSocket(parent),
	d(0)
{
}

WebSocketOverHttp::~WebSocketOverHttp()
{
	if(d->state == Connected && parent() != g_disconnectManager)
	{
		// if we get destructed while connected, disconnect in the background
		WebSocketOverHttp *sock = new WebSocketOverHttp;
		sock->d = d;
		d->setParent(sock);
		d->q = sock;
		d = 0;
		g_disconnectManager->addSocket(sock);
	}

	delete d;
}

void WebSocketOverHttp::setConnectionId(const QByteArray &id)
{
	d->cid = id;
}

void WebSocketOverHttp::clearDisconnectManager()
{
	delete g_disconnectManager;
	g_disconnectManager = 0;
}

void WebSocketOverHttp::sendDisconnect()
{
	d->sendDisconnect();
}

QHostAddress WebSocketOverHttp::peerAddress() const
{
	// this class is client only
	return QHostAddress();
}

void WebSocketOverHttp::setConnectHost(const QString &host)
{
	d->connectHost = host;
}

void WebSocketOverHttp::setConnectPort(int port)
{
	d->connectPort = port;
}

void WebSocketOverHttp::setIgnorePolicies(bool on)
{
	d->ignorePolicies = on;
}

void WebSocketOverHttp::setIgnoreTlsErrors(bool on)
{
	d->ignoreTlsErrors = on;
}

void WebSocketOverHttp::start(const QUrl &uri, const HttpHeaders &headers)
{
	assert(d->state == Idle);

	d->requestData.uri = uri;
	d->requestData.headers = headers;
	d->start();
}

void WebSocketOverHttp::respondSuccess(const QByteArray &reason, const HttpHeaders &headers)
{
	Q_UNUSED(reason);
	Q_UNUSED(headers);

	// this class is client only
	assert(0);
}

void WebSocketOverHttp::respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
{
	Q_UNUSED(code);
	Q_UNUSED(reason);
	Q_UNUSED(headers);
	Q_UNUSED(body);

	// this class is client only
	assert(0);
}

WebSocket::State WebSocketOverHttp::state() const
{
	return d->state;
}

QUrl WebSocketOverHttp::requestUri() const
{
	return d->requestData.uri;
}

HttpHeaders WebSocketOverHttp::requestHeaders() const
{
	return d->requestData.headers;
}

int WebSocketOverHttp::responseCode() const
{
	return d->responseData.code;
}

QByteArray WebSocketOverHttp::responseReason() const
{
	return d->responseData.reason;
}

HttpHeaders WebSocketOverHttp::responseHeaders() const
{
	return d->responseData.headers;
}

QByteArray WebSocketOverHttp::responseBody() const
{
	return d->responseData.body;
}

int WebSocketOverHttp::framesAvailable() const
{
	return d->inFrames.count();
}

bool WebSocketOverHttp::canWrite() const
{
	return (writeBytesAvailable() > 0);
}

int WebSocketOverHttp::writeBytesAvailable() const
{
	return d->writeBytesAvailable();
}

int WebSocketOverHttp::peerCloseCode() const
{
	return d->peerCloseCode;
}

WebSocket::ErrorCondition WebSocketOverHttp::errorCondition() const
{
	return d->errorCondition;
}

void WebSocketOverHttp::writeFrame(const Frame &frame)
{
	d->writeFrame(frame);
}

WebSocket::Frame WebSocketOverHttp::readFrame()
{
	return d->readFrame();
}

void WebSocketOverHttp::close(int code)
{
	d->close(code);
}

#include "websocketoverhttp.moc"
