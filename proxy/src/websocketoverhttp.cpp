/*
 * Copyright (C) 2014 Fanout, Inc.
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
#include <QUuid>
#include "log.h"
#include "bufferlist.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "zhttprequest.h"
#include "zhttpmanager.h"

#define BUFFER_SIZE 200000

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
	HttpRequestData requestData;
	HttpResponseData responseData;
	ErrorCondition errorCondition;
	ZhttpRequest *req;
	int reqFrames;
	int reqContentSize;
	bool reqClose;
	BufferList inBuf;
	QList<Frame> inFrames;
	QList<Frame> outFrames;
	int closeCode;
	bool peerClosing;
	int peerCloseCode;

	Private(WebSocketOverHttp *_q) :
		QObject(_q),
		q(_q),
		connectPort(-1),
		ignorePolicies(false),
		ignoreTlsErrors(false),
		state(WebSocket::Idle),
		errorCondition(WebSocket::ErrorGeneric),
		req(0),
		reqFrames(0),
		reqContentSize(0),
		reqClose(false),
		closeCode(-1),
		peerClosing(false),
		peerCloseCode(-1)
	{
	}

	void start()
	{
		state = Connecting;

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

private:
	void update()
	{
		// only one request allowed at a time
		if(req)
			return;

		requestData.headers.removeAll("Upgrade");

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

		req->start("POST", requestData.uri, requestData.headers);

		reqFrames = 0;
		reqContentSize = 0;
		reqClose = false;

		QList<WsEvent> events;

		if(state == Connecting)
		{
			events += WsEvent("OPEN");
		}
		else
		{
			while(!outFrames.isEmpty())
			{
				Frame f = outFrames.takeFirst();
				if(f.type == Frame::Text)
					events += WsEvent("TEXT", f.data);
				else if(f.type == Frame::Binary)
					events += WsEvent("BINARY", f.data);
				else if(f.type == Frame::Ping)
					events += WsEvent("PING");
				else if(f.type == Frame::Pong)
					events += WsEvent("PONG");

				++reqFrames;
				reqContentSize += f.data.size();
			}

			if(state == Closing)
			{
				QByteArray buf(2, 0);
				buf[0] = (closeCode >> 8) & 0xff;
				buf[1] = closeCode & 0xff;
				events += WsEvent("CLOSE", buf);

				reqClose = true;
			}
		}

		req->writeBody(encodeEvents(events));
		req->endBody();
	}

private slots:
	// FIXME: DOR-SS
	void req_readyRead()
	{
		inBuf += req->readBody();
		if(req->isFinished())
		{
			// TODO: Keep-Alive-Interval
			// TODO: Meta-Set stuff
			if(state == Connecting)
			{
				responseData.code = req->responseCode();
				responseData.reason = req->responseReason();
				responseData.headers = req->responseHeaders();
			}

			delete req;
			req = 0;

			// TODO: check for error
			QList<WsEvent> events = decodeEvents(inBuf.take());

			foreach(const WsEvent &e, events)
			{
				if(e.type == "OPEN")
				{
					state = Connected;
					emit q->connected();
				}
				else if(e.type == "TEXT")
				{
					inFrames += Frame(Frame::Text, e.content, false);
					emit q->readyRead();
				}
				else if(e.type == "BINARY")
				{
					inFrames += Frame(Frame::Binary, e.content, false);
					emit q->readyRead();
				}
				else if(e.type == "PING")
				{
					inFrames += Frame(Frame::Ping, QByteArray(), false);
					emit q->readyRead();
				}
				else if(e.type == "PONG")
				{
					inFrames += Frame(Frame::Pong, QByteArray(), false);
					emit q->readyRead();
				}
				else if(e.type == "CLOSE")
				{
					if(state == Closing)
					{
						state = Idle;
						emit q->closed();
						return;
					}
					else
					{
						peerClosing = true;
						if(e.content.size() == 2)
							peerCloseCode = ((quint16)e.content[0] << 8) + (quint16)e.content[1];

						emit q->peerClosed();
					}
				}
				else if(e.type == "DISCONNECT")
				{
					emit q->error();
					return;
				}
			}

			if(peerClosing && reqClose)
			{
				state = Idle;
				emit q->closed();
				return;
			}

			emit q->framesWritten(reqFrames, reqContentSize);

			if(!outFrames.isEmpty())
				update();
		}
	}

	void req_bytesWritten(int count)
	{
		Q_UNUSED(count);

		// nothing to do here
	}

	void req_error()
	{
		// TODO
		emit q->error();
	}
};

WebSocketOverHttp::WebSocketOverHttp(ZhttpManager *zhttpManager, QObject *parent) :
	WebSocket(parent)
{
	d = new Private(this);
	d->zhttpManager = zhttpManager;
}

WebSocketOverHttp::~WebSocketOverHttp()
{
	delete d;
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
	int avail = BUFFER_SIZE;
	foreach(const Frame &f, d->outFrames)
	{
		if(f.data.size() >= avail)
			return 0;

		avail -= f.data.size();
	}

	return avail;
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
