/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $FANOUT_END_LICENSE$
 */

#include "testwebsocket.h"

#include <assert.h>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include "defercall.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "statusreasons.h"

#define BUFFER_SIZE 200000

class TestWebSocket::Private
{
public:
	enum State
	{
		Idle,
		Connecting,
		Connected,
		Closing
	};

	TestWebSocket *q;
	State state;
	HttpRequestData request;
	HttpResponseData response;
	bool gripEnabled;
	QList<Frame> inFrames;
	int peerCloseCode;
	QString peerCloseReason;
	ErrorCondition errorCondition;
	DeferCall deferCall;

	Private(TestWebSocket *_q) :
		q(_q),
		state(Idle),
		gripEnabled(false),
		peerCloseCode(-1),
		errorCondition(ErrorGeneric)
	{
	}

	void handleConnect()
	{
		QString path = request.uri.path();
		if(path.length() >= 2 && path.endsWith('/'))
			path.truncate(path.length() - 1);

		QSet<QString> channels;

		QUrlQuery query(request.uri);
		QList<QPair<QString, QString> > queryItems = query.queryItems();
		for(int n = 0; n < queryItems.count(); ++n)
		{
			if(queryItems[n].first == "channel")
				channels += queryItems[n].second;
		}

		if(channels.isEmpty())
			channels += "test";

		if(path == "/ws")
		{
			state = Connected;
			response.code = 101;
			response.reason = StatusReasons::getReason(response.code);

			gripEnabled = false;
			foreach(const HttpHeaderParameters &ext, request.headers.getAllAsParameters("Sec-WebSocket-Extensions"))
			{
				if(!ext.isEmpty() && ext[0].first == "grip")
				{
					gripEnabled = true;
					break;
				}
			}

			if(gripEnabled)
			{
				response.headers += HttpHeader("Sec-WebSocket-Extensions", "grip");

				foreach(const QString &channel, channels)
				{
					QJsonObject obj;
					obj["type"] = QString("subscribe");
					obj["channel"] = channel;
					inFrames += Frame(Frame::Text, QByteArray("c:") + QJsonDocument(obj).toJson(), false);
				}
			}

			q->connected();

			if(gripEnabled && !channels.isEmpty())
				deferCall.defer([=] { doReadyRead(); });
		}
		else
		{
			response.code = 404;
			response.reason = StatusReasons::getReason(response.code);
			response.headers += HttpHeader("Content-Type", "text/plain");
			response.body += QByteArray("no such test resource\n");

			errorCondition = ErrorRejected;
			q->error();
		}
	}

	void doReadyRead()
	{
		q->readyRead();
	}

	void doFramesWritten(int count, int bytes)
	{
		q->framesWritten(count, bytes);
	}

	void doWriteBytesChanged()
	{
		q->writeBytesChanged();
	}

	void handleClose()
	{
		state = Idle;
		q->closed();
	}
};

TestWebSocket::TestWebSocket()
{
	d = new Private(this);
}

TestWebSocket::~TestWebSocket()
{
	delete d;
}

QHostAddress TestWebSocket::peerAddress() const
{
	// this class is client only
	return QHostAddress();
}

void TestWebSocket::setConnectHost(const QString &host)
{
	Q_UNUSED(host);
}

void TestWebSocket::setConnectPort(int port)
{
	Q_UNUSED(port);
}

void TestWebSocket::setIgnorePolicies(bool on)
{
	Q_UNUSED(on);
}

void TestWebSocket::setTrustConnectHost(bool on)
{
	Q_UNUSED(on);
}

void TestWebSocket::setIgnoreTlsErrors(bool on)
{
	Q_UNUSED(on);
}

void TestWebSocket::start(const QUrl &uri, const HttpHeaders &headers)
{
	d->request.uri = uri;
	d->request.headers = headers;

	d->state = Private::Connecting;

	d->deferCall.defer([=] { d->handleConnect(); });
}

void TestWebSocket::respondSuccess(const QByteArray &reason, const HttpHeaders &headers)
{
	Q_UNUSED(reason);
	Q_UNUSED(headers);

	// this class is client only
	assert(0);
}

void TestWebSocket::respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
{
	Q_UNUSED(code);
	Q_UNUSED(reason);
	Q_UNUSED(headers);
	Q_UNUSED(body);

	// this class is client only
	assert(0);
}

WebSocket::State TestWebSocket::state() const
{
	if(d->state == Private::Idle)
		return Idle;
	else if(d->state == Private::Connecting)
		return Connecting;
	else if(d->state == Private::Connected)
		return Connected;
	else // Private::Closing
		return Closing;
}

QUrl TestWebSocket::requestUri() const
{
	return d->request.uri;
}

HttpHeaders TestWebSocket::requestHeaders() const
{
	return d->request.headers;
}

int TestWebSocket::responseCode() const
{
	return d->response.code;
}

QByteArray TestWebSocket::responseReason() const
{
	return d->response.reason;
}

HttpHeaders TestWebSocket::responseHeaders() const
{
	return d->response.headers;
}

QByteArray TestWebSocket::responseBody() const
{
	return d->response.body;
}

int TestWebSocket::framesAvailable() const
{
	return d->inFrames.count();
}

int TestWebSocket::writeBytesAvailable() const
{
	int inSize = 0;
	foreach(const Frame &f, d->inFrames)
		inSize += f.data.size();

	if(inSize < BUFFER_SIZE)
		return BUFFER_SIZE - inSize;
	else
		return 0;
}

int TestWebSocket::peerCloseCode() const
{
	return d->peerCloseCode;
}

QString TestWebSocket::peerCloseReason() const
{
	return d->peerCloseReason;
}

WebSocket::ErrorCondition TestWebSocket::errorCondition() const
{
	return d->errorCondition;
}

void TestWebSocket::writeFrame(const Frame &frame)
{
	Frame tmp = frame;

	if(d->gripEnabled && (frame.type == Frame::Text || frame.type == Frame::Binary))
	{
		tmp.data = "m:" + tmp.data;
	}

	d->inFrames += tmp;

	int contentBytesWritten = tmp.data.size();
	d->deferCall.defer([=] { d->doFramesWritten(1, contentBytesWritten); });
	d->deferCall.defer([=] { d->doReadyRead(); });
}

WebSocket::Frame TestWebSocket::readFrame()
{
	d->deferCall.defer([=] { d->doWriteBytesChanged(); });

	return d->inFrames.takeFirst();
}

void TestWebSocket::close(int code, const QString &reason)
{
	d->state = Private::Closing;
	d->peerCloseCode = code;
	d->peerCloseReason = reason;

	d->deferCall.defer([=] { d->handleClose(); });
}
