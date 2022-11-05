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

#include "testhttprequest.h"

#include <assert.h>
#include <QUrlQuery>
#include "log.h"
#include "bufferlist.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "statusreasons.h"

#define MAX_REQUEST_SIZE 100000

class TestHttpRequest::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Idle,
		ReceivingRequest,
		Responding,
		Responded
	};

	TestHttpRequest *q;
	State state;
	HttpRequestData request;
	HttpResponseData response;
	BufferList requestBody;
	bool requestBodyFinished;
	BufferList responseBody;
	ErrorCondition errorCondition;

	Private(TestHttpRequest *_q) :
		QObject(_q),
		q(_q),
		state(Idle),
		requestBodyFinished(false),
		errorCondition(ErrorGeneric)
	{
	}

public slots:
	void processRequest()
	{
		if(!requestBodyFinished)
		{
			response.code = 400;
			response.reason = StatusReasons::getReason(response.code);
			response.headers += HttpHeader("Content-Type", "text/plain");
			responseBody += QByteArray("request too large\n");

			state = Responded;
			emit q->readyRead();
			return;
		}

		log_debug("processing test request: %s", qPrintable(request.uri.path()));

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

		if(path == "/")
		{
			response.code = 200;
			response.reason = StatusReasons::getReason(response.code);
			response.headers += HttpHeader("Content-Type", "text/plain");
			responseBody += QByteArray("Hello from the Pushpin test handler!\n");
		}
		else if(path == "/response")
		{
			response.code = 200;
			response.reason = StatusReasons::getReason(response.code);
			response.headers += HttpHeader("Content-Type", "text/plain");
			response.headers += HttpHeader("Grip-Hold", "response");
			response.headers += HttpHeader("Grip-Channel", QStringList(channels.toList()).join(", ").toUtf8());
			responseBody += QByteArray("nothing for now\n");
		}
		else if(path == "/stream")
		{
			response.code = 200;
			response.reason = StatusReasons::getReason(response.code);
			response.headers += HttpHeader("Content-Type", "text/plain");
			response.headers += HttpHeader("Grip-Hold", "stream");
			response.headers += HttpHeader("Grip-Channel", QStringList(channels.toList()).join(", ").toUtf8());
			responseBody += QByteArray("[stream opened]\n");
		}
		else
		{
			response.code = 404;
			response.reason = StatusReasons::getReason(response.code);
			response.headers += HttpHeader("Content-Type", "text/plain");
			responseBody += QByteArray("no such test resource\n");
		}

		state = Responded;
		emit q->readyRead();
	}
};

TestHttpRequest::TestHttpRequest(QObject *parent) :
	HttpRequest(parent)
{
	d = new Private(this);
}

TestHttpRequest::~TestHttpRequest()
{
	delete d;
}

QHostAddress TestHttpRequest::peerAddress() const
{
	// this class is client only
	return QHostAddress();
}

void TestHttpRequest::setConnectHost(const QString &host)
{
	Q_UNUSED(host);
}

void TestHttpRequest::setConnectPort(int port)
{
	Q_UNUSED(port);
}

void TestHttpRequest::setIgnorePolicies(bool on)
{
	Q_UNUSED(on);
}

void TestHttpRequest::setTrustConnectHost(bool on)
{
	Q_UNUSED(on);
}

void TestHttpRequest::setIgnoreTlsErrors(bool on)
{
	Q_UNUSED(on);
}

void TestHttpRequest::start(const QString &method, const QUrl &uri, const HttpHeaders &headers)
{
	assert(d->state == Private::Idle);

	d->state = Private::ReceivingRequest;

	d->request.method = method;
	d->request.uri = uri;
	d->request.headers = headers;
}

void TestHttpRequest::beginResponse(int code, const QByteArray &reason, const HttpHeaders &headers)
{
	Q_UNUSED(code);
	Q_UNUSED(reason);
	Q_UNUSED(headers);

	// this class is client only
	assert(0);
}

void TestHttpRequest::writeBody(const QByteArray &body)
{
	if(d->state == Private::ReceivingRequest)
	{
		if(d->requestBody.size() + body.size() > MAX_REQUEST_SIZE)
		{
			d->state = Private::Responding;
			QMetaObject::invokeMethod(d, "processRequest", Qt::QueuedConnection);
			return;
		}

		QByteArray buf = body.mid(0, MAX_REQUEST_SIZE - d->requestBody.size());

		if(!buf.isEmpty())
		{
			d->requestBody += buf;

			QMetaObject::invokeMethod(this, "bytesWritten", Qt::QueuedConnection, Q_ARG(int, buf.size()));
		}
	}
}

void TestHttpRequest::endBody()
{
	if(d->state == Private::ReceivingRequest)
	{
		d->requestBodyFinished = true;

		d->state = Private::Responding;
		QMetaObject::invokeMethod(d, "processRequest", Qt::QueuedConnection);
	}
}

int TestHttpRequest::bytesAvailable() const
{
	return d->responseBody.size();
}

int TestHttpRequest::writeBytesAvailable() const
{
	return (MAX_REQUEST_SIZE - d->requestBody.size() + 1);
}

bool TestHttpRequest::isFinished() const
{
	return (d->state == Private::Responded);
}

bool TestHttpRequest::isInputFinished() const
{
	return (d->state == Private::Responded);
}

bool TestHttpRequest::isOutputFinished() const
{
	return d->requestBodyFinished;
}

bool TestHttpRequest::isErrored() const
{
	// this class can't fail
	return false;
}

HttpRequest::ErrorCondition TestHttpRequest::errorCondition() const
{
	return d->errorCondition;
}

QString TestHttpRequest::requestMethod() const
{
	return d->request.method;
}

QUrl TestHttpRequest::requestUri() const
{
	return d->request.uri;
}

HttpHeaders TestHttpRequest::requestHeaders() const
{
	return d->request.headers;
}

int TestHttpRequest::responseCode() const
{
	return d->response.code;
}

QByteArray TestHttpRequest::responseReason() const
{
	return d->response.reason;
}

HttpHeaders TestHttpRequest::responseHeaders() const
{
	return d->response.headers;
}

QByteArray TestHttpRequest::readBody(int size)
{
	return d->responseBody.take(size);
}

#include "testhttprequest.moc"
