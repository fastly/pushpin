/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "testwebsocket.h"

#include <assert.h>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "statusreasons.h"

#define BUFFER_SIZE 200000

class TestWebSocket::Private : public QObject
{
	Q_OBJECT

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

	Private(TestWebSocket *_q) :
		QObject(_q),
		q(_q),
		state(Idle),
		gripEnabled(false),
		peerCloseCode(-1),
		errorCondition(ErrorGeneric)
	{
	}

public slots:
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

			emit q->connected();

			if(gripEnabled && !channels.isEmpty())
				QMetaObject::invokeMethod(q, "readyRead", Qt::QueuedConnection);
		}
		else
		{
			response.code = 404;
			response.reason = StatusReasons::getReason(response.code);
			response.headers += HttpHeader("Content-Type", "text/plain");
			response.body += QByteArray("no such test resource\n");

			errorCondition = ErrorRejected;
			emit q->error();
		}
	}

	void handleClose()
	{
		state = Idle;
		emit q->closed();
	}
};

TestWebSocket::TestWebSocket(QObject *parent) :
	WebSocket(parent)
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

	QMetaObject::invokeMethod(d, "handleConnect", Qt::QueuedConnection);
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

bool TestWebSocket::canWrite() const
{
	return (writeBytesAvailable() > 0);
}

int TestWebSocket::writeBytesAvailable() const
{
	int avail = BUFFER_SIZE;
	foreach(const Frame &f, d->inFrames)
	{
		if(f.data.size() >= avail)
			return 0;

		avail -= f.data.size();
	}

	return avail;
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

	QMetaObject::invokeMethod(this, "framesWritten", Qt::QueuedConnection, Q_ARG(int, 1), Q_ARG(int, tmp.data.size()));
	QMetaObject::invokeMethod(this, "readyRead", Qt::QueuedConnection);
}

WebSocket::Frame TestWebSocket::readFrame()
{
	return d->inFrames.takeFirst();
}

void TestWebSocket::close(int code, const QString &reason)
{
	d->state = Private::Closing;
	d->peerCloseCode = code;
	d->peerCloseReason = reason;

	QMetaObject::invokeMethod(d, "handleClose", Qt::QueuedConnection);
}

#include "testwebsocket.moc"
