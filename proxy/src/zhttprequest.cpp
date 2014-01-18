/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#include "zhttprequest.h"

#include <assert.h>
#include <QTimer>
#include <QPointer>
#include <QUuid>
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "bufferlist.h"
#include "log.h"
#include "zhttpmanager.h"

#define IDEAL_CREDITS 200000
#define SESSION_EXPIRE 60000

class ZhttpRequest::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Stopped,                 // response finished, error, or not even started

		ClientStarting,          // prepared to send the first packet
		ClientRequestStartWait,  // sent the first packet of streamed input, waiting for ack
		ClientRequesting,        // sending the rest of streamed input
		ClientRequestFinishWait, // completed sending the request, waiting for ack
		ClientReceiving,         // completed sending the request, waiting on response

		ServerStarting,          // prepared to process the first packet
		ServerReceiving,         // receiving the rest of streamed input
		ServerResponseWait,      // waiting for the response to start
		ServerResponseStarting,  // about to send the first packet
		ServerResponding         // sending the response
	};

	ZhttpRequest *q;
	ZhttpManager *manager;
	bool server;
	State state;
	ZhttpRequest::Rid rid;
	QByteArray toAddress;
	QHostAddress peerAddress;
	QString connectHost;
	int connectPort;
	bool ignorePolicies;
	bool ignoreTlsErrors;
	QString requestMethod;
	QUrl requestUri;
	HttpHeaders requestHeaders;
	BufferList requestBodyBuf;
	int inSeq;
	int outSeq;
	int outCredits;
	bool bodyFinished; // user has finished providing input
	int pendingInCredits;
	bool haveRequestBody;
	bool haveResponseValues;
	int responseCode;
	QByteArray responseReason;
	HttpHeaders responseHeaders;
	BufferList responseBodyBuf;
	QVariant userData;
	bool pausing;
	bool paused;
	bool pendingUpdate;
	ZhttpRequest::ErrorCondition errorCondition;
	QTimer *expireTimer;
	QTimer *keepAliveTimer;

	Private(ZhttpRequest *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		server(false),
		state(Stopped),
		connectPort(-1),
		ignorePolicies(false),
		ignoreTlsErrors(false),
		inSeq(0),
		outSeq(0),
		outCredits(0),
		bodyFinished(false),
		pendingInCredits(0),
		haveRequestBody(false),
		haveResponseValues(false),
		pausing(false),
		paused(false),
		pendingUpdate(false),
		expireTimer(0),
		keepAliveTimer(0)
	{
		expireTimer = new QTimer(this);
		connect(expireTimer, SIGNAL(timeout()), this, SLOT(expire_timeout()));
		expireTimer->setSingleShot(true);

		keepAliveTimer = new QTimer(this);
		connect(keepAliveTimer, SIGNAL(timeout()), this, SLOT(keepAlive_timeout()));
	}

	~Private()
	{
		if(manager && !paused && state != Stopped)
			tryCancel();

		cleanup();
	}

	void cleanup()
	{
		if(expireTimer)
		{
			expireTimer->disconnect(this);
			expireTimer->setParent(0);
			expireTimer->deleteLater();
			expireTimer = 0;
		}

		if(keepAliveTimer)
		{
			keepAliveTimer->disconnect(this);
			keepAliveTimer->setParent(0);
			keepAliveTimer->deleteLater();
			keepAliveTimer = 0;
		}

		if(manager)
		{
			manager->unlink(q);
			manager = 0;
		}
	}

	bool setupServer(const ZhttpRequestPacket &packet)
	{
		if(packet.type != ZhttpRequestPacket::Data)
		{
			log_warning("zhttp server: received request with invalid type, canceling");
			tryRespondCancel(packet);
			return false;
		}

		if(packet.seq != -1 && packet.seq != 0)
		{
			log_warning("zhttp server: error, received request with non-zero seq field");
			writeError("bad-request");
			state = Stopped;
			return false;
		}

		if(!packet.stream)
		{
			log_warning("zhttp server: error, received request for non-stream response");
			writeError("bad-request");
			state = Stopped;
			return false;
		}

		if(packet.seq == -1 && packet.more)
		{
			log_warning("zhttp server: error, received stream request with no seq field");
			writeError("bad-request");
			state = Stopped;
			return false;
		}

		inSeq = 1; // next expected seq

		if(packet.credits != -1)
			outCredits = packet.credits;

		requestMethod = packet.method;
		requestUri = packet.uri;
		requestHeaders = packet.headers;
		requestBodyBuf += packet.body;

		userData = packet.userData;
		peerAddress = packet.peerAddress;

		if(!packet.more)
			haveRequestBody = true;

		return true;
	}

	void setupServer(const ZhttpRequest::ServerState &ss)
	{
		peerAddress = ss.peerAddress;
		requestMethod = ss.requestMethod;
		requestUri = ss.requestUri;
		requestHeaders = ss.requestHeaders;
		if(ss.inSeq >= 0)
			inSeq = ss.inSeq;
		if(ss.outSeq >= 0)
			outSeq = ss.outSeq;
		if(ss.outCredits >= 0)
			outCredits = ss.outCredits;
		userData = ss.userData;

		state = ServerResponseWait;

		refreshTimeout();
		startKeepAlive();

		// send a keep-alive right away to accept after handoff
		ZhttpResponsePacket p;
		p.type = ZhttpResponsePacket::KeepAlive;
		writePacket(p);
	}

	void startClient()
	{
		state = ClientStarting;

		refreshTimeout();
		update();
	}

	void startServer()
	{
		state = ServerStarting;

		startKeepAlive();
		refreshTimeout();
		update();
	}

	void pause()
	{
		pausing = true;

		ZhttpResponsePacket p;
		p.type = ZhttpResponsePacket::HandoffStart;
		writePacket(p);
	}

	void beginResponse()
	{
		assert(!pausing && !paused);
		state = ServerResponseStarting;
		update();
	}

	void startKeepAlive()
	{
		keepAliveTimer->start(SESSION_EXPIRE / 2);
	}

	void refreshTimeout()
	{
		expireTimer->start(SESSION_EXPIRE);
	}

	void update()
	{
		if(!pendingUpdate)
		{
			pendingUpdate = true;
			QMetaObject::invokeMethod(this, "doUpdate", Qt::QueuedConnection);
		}
	}

	QByteArray readBody(int size)
	{
		if(server)
		{
			QByteArray out = requestBodyBuf.take(size);
			if(out.isEmpty())
				return out;

			pendingInCredits += out.size();

			if(!pausing && !paused)
			{
				ZhttpResponsePacket p;
				p.type = ZhttpResponsePacket::Credit;
				p.credits = pendingInCredits;
				pendingInCredits = 0;
				writePacket(p);
			}

			return out;
		}
		else
		{
			QByteArray out = responseBodyBuf.take(size);
			if(out.isEmpty())
				return out;

			pendingInCredits += out.size();

			if(state == ClientReceiving)
				tryWrite(); // this should not emit signals in current state

			return out;
		}
	}

	void tryWrite()
	{
		if(state == ClientRequesting)
		{
			// if all we have to send is EOF, we don't need credits for that
			if(requestBodyBuf.isEmpty() && bodyFinished)
			{
				state = ClientReceiving;

				ZhttpRequestPacket p;
				p.type = ZhttpRequestPacket::Data;
				writePacket(p);

				emit q->bytesWritten(0);
			}
			else if(!requestBodyBuf.isEmpty() && outCredits > 0)
			{
				// if we have data to send, and the credits to do so, then send data.
				// also send credits if we need to.

				QByteArray buf = requestBodyBuf.take(outCredits);
				outCredits -= buf.size();

				ZhttpRequestPacket p;
				p.type = ZhttpRequestPacket::Data;
				p.body = buf;
				if(!requestBodyBuf.isEmpty() || !bodyFinished)
					p.more = true;
				if(pendingInCredits > 0)
				{
					p.credits = pendingInCredits;
					pendingInCredits = 0;
				}

				if(!p.more)
					state = ClientReceiving;

				writePacket(p);

				emit q->bytesWritten(buf.size());
			}
		}
		else if(state == ClientReceiving)
		{
			if(pendingInCredits > 0)
			{
				// if we have no data to send but we need to send credits, do at least that
				ZhttpRequestPacket p;
				p.type = ZhttpRequestPacket::Credit;
				p.credits = pendingInCredits;
				pendingInCredits = 0;

				writePacket(p);
			}
		}
		else if(state == ServerResponding)
		{
			if((!responseBodyBuf.isEmpty() && outCredits > 0) || (responseBodyBuf.isEmpty() && bodyFinished))
			{
				ZhttpResponsePacket packet;
				packet.body = responseBodyBuf.take(outCredits);
				outCredits -= packet.body.size();
				packet.more = (!responseBodyBuf.isEmpty() || !bodyFinished);

				writePacket(packet);

				if(!packet.more)
				{
					state = Stopped;
					cleanup();
				}

				emit q->bytesWritten(packet.body.size());
			}
		}
	}

	void handle(const ZhttpRequestPacket &packet)
	{
		if(paused)
			return;

		if(packet.type == ZhttpRequestPacket::Error)
		{
			errorCondition = convertError(packet.condition);

			log_debug("zhttp server: error id=%s cond=%s", packet.id.data(), packet.condition.data());

			state = Stopped;
			cleanup();
			emit q->error();
			return;
		}
		else if(packet.type == ZhttpRequestPacket::Cancel)
		{
			log_debug("zhttp server: received cancel id=%s", packet.id.data());

			errorCondition = ErrorGeneric;
			state = Stopped;
			cleanup();
			emit q->error();
			return;
		}

		if(packet.seq != inSeq)
		{
			log_warning("zhttp server: error id=%s received message out of sequence, canceling", packet.id.data());

			// if this was not an error packet, send cancel
			if(packet.type != ZhttpRequestPacket::Error && packet.type != ZhttpRequestPacket::Cancel)
			{
				ZhttpResponsePacket p;
				p.type = ZhttpResponsePacket::Cancel;
				writePacket(p);
			}

			state = Stopped;
			errorCondition = ErrorGeneric;
			cleanup();
			emit q->error();
			return;
		}

		++inSeq;

		refreshTimeout();

		if(packet.type == ZhttpRequestPacket::Data)
		{
			requestBodyBuf += packet.body;

			bool done = haveRequestBody;

			if(!packet.more)
			{
				haveRequestBody = true;
				state = ServerResponseWait;
			}

			if(packet.credits > 0)
				outCredits += packet.credits;

			if(!packet.body.isEmpty() || (!done && haveRequestBody))
				emit q->readyRead();
		}
		else if(packet.type == ZhttpRequestPacket::Credit)
		{
			if(packet.credits > 0)
			{
				outCredits += packet.credits;
				tryWrite();
			}
		}
		else if(packet.type == ZhttpRequestPacket::KeepAlive)
		{
			// nothing to do
		}
		else if(packet.type == ZhttpRequestPacket::HandoffProceed)
		{
			if(pausing)
			{
				pausing = false;
				paused = true;
				emit q->paused();
			}
		}
		else
		{
			log_debug("zhttp server: unsupported packet type id=%s type=%d", packet.id.data(), (int)packet.type);
		}
	}

	void handle(const ZhttpResponsePacket &packet)
	{
		if(state == ClientRequestStartWait)
		{
			if(packet.from.isEmpty())
			{
				state = Stopped;
				errorCondition = ErrorGeneric;
				cleanup();
				log_warning("zhttp client: error id=%s initial ack for streamed input request did not contain from field", packet.id.data());
				emit q->error();
				return;
			}

			toAddress = packet.from;

			state = ClientRequesting;

			startKeepAlive();
		}
		else if(state == ClientRequestFinishWait)
		{
			toAddress = packet.from;

			state = ClientReceiving;

			startKeepAlive();
		}

		if(packet.type == ZhttpResponsePacket::Error)
		{
			errorCondition = convertError(packet.condition);

			log_debug("zhttp client: error id=%s cond=%s", packet.id.data(), packet.condition.data());

			state = Stopped;
			cleanup();
			emit q->error();
			return;
		}
		else if(packet.type == ZhttpResponsePacket::Cancel)
		{
			log_debug("zhttp client: received cancel id=%s", packet.id.data());

			errorCondition = ErrorGeneric;
			state = Stopped;
			cleanup();
			emit q->error();
			return;
		}

		if(packet.seq != inSeq)
		{
			log_warning("zhttp client: error id=%s received message out of sequence, canceling", packet.id.data());

			// if this was not an error packet, send cancel
			if(packet.type != ZhttpResponsePacket::Error && packet.type != ZhttpResponsePacket::Cancel)
			{
				ZhttpRequestPacket p;
				p.type = ZhttpRequestPacket::Cancel;
				writePacket(p);
			}

			state = Stopped;
			errorCondition = ErrorGeneric;
			cleanup();
			emit q->error();
			return;
		}

		++inSeq;

		refreshTimeout();

		if(packet.type == ZhttpResponsePacket::Data)
		{
			if(!haveResponseValues)
			{
				haveResponseValues = true;

				responseCode = packet.code;
				responseReason = packet.reason;
				responseHeaders = packet.headers;
			}

			if(responseBodyBuf.size() + packet.body.size() > IDEAL_CREDITS)
				log_warning("zhttp client: id=%s server is sending too fast", packet.id.data());

			responseBodyBuf += packet.body;

			if(packet.credits > 0)
			{
				outCredits += packet.credits;
				if(outCredits > 0)
				{
					// try to write anything that was waiting on credits
					QPointer<QObject> self = this;
					tryWrite();
					if(!self)
						return;
				}
			}

			if(packet.more)
			{
				if(!packet.body.isEmpty())
					emit q->readyRead();
			}
			else
			{
				// always emit readyRead here even if body is empty, for EOF
				state = Stopped;
				cleanup();
				emit q->readyRead();
			}
		}
		else if(packet.type == ZhttpResponsePacket::Credit)
		{
			if(packet.credits > 0)
			{
				outCredits += packet.credits;
				if(outCredits > 0)
					tryWrite();
			}
		}
		else if(packet.type == ZhttpResponsePacket::KeepAlive)
		{
			// nothing to do
		}
		else
		{
			log_debug("zhttp server: unsupported packet type id=%s type=%d", packet.id.data(), (int)packet.type);
		}
	}

	void writeBody(const QByteArray &body)
	{
		assert(!bodyFinished);
		assert(!pausing && !paused);

		if(server)
			responseBodyBuf += body;
		else
			requestBodyBuf += body;

		update();
	}

	void endBody()
	{
		assert(!bodyFinished);
		assert(!pausing && !paused);

		bodyFinished = true;
		update();
	}

	void writePacket(const ZhttpRequestPacket &packet)
	{
		assert(manager);

		ZhttpRequestPacket out = packet;
		out.from = rid.first;
		out.id = rid.second;
		out.seq = outSeq++;
		
		if(out.seq == 0)
		{
			manager->write(out);
		}
		else
		{
			assert(!toAddress.isEmpty());
			manager->write(out, toAddress);
		}
	}

	void writePacket(const ZhttpResponsePacket &packet)
	{
		assert(manager);

		ZhttpResponsePacket out = packet;
		out.from = manager->instanceId();
		out.id = rid.second;
		out.seq = outSeq++;
		out.userData = userData;
		
		manager->write(out, rid.first);
	}

	void writeCancel()
	{
		ZhttpResponsePacket out;
		out.type = ZhttpResponsePacket::Cancel;
		writePacket(out);
	}

	void writeError(const QByteArray &condition)
	{
		ZhttpResponsePacket out;
		out.type = ZhttpResponsePacket::Error;
		out.condition = condition;
		writePacket(out);
	}

	void tryCancel()
	{
		if(state == ClientRequesting || state == ClientReceiving)
		{
			state = Stopped;

			ZhttpRequestPacket p;
			p.type = ZhttpRequestPacket::Cancel;
			writePacket(p);
		}
		else if(server)
		{
			state = Stopped;

			ZhttpResponsePacket p;
			p.type = ZhttpResponsePacket::Cancel;
			writePacket(p);
		}
	}

	void tryRespondCancel(const ZhttpRequestPacket &packet)
	{
		// if this was not an error packet, send cancel
		if(packet.type != ZhttpRequestPacket::Error && packet.type != ZhttpRequestPacket::Cancel)
			writeCancel();
	}

	static ErrorCondition convertError(const QByteArray &cond)
	{
		// zhttp conditions:
		//  remote-connection-failed
		//  connection-timeout
		//  tls-error
		//  bad-request
		//  policy-violation
		//  max-size-exceeded
		//  session-timeout
		//  cancel

		if(cond == "policy-violation")
			return ErrorPolicy;
		else if(cond == "remote-connection-failed")
			return ErrorConnect;
		else if(cond == "tls-error")
			return ErrorTls;
		else if(cond == "length-required")
			return ErrorLengthRequired;
		else if(cond == "connection-timeout")
			return ErrorConnectTimeout;
		else // lump the rest as generic
			return ErrorGeneric;
	}

public slots:
	void doUpdate()
	{
		pendingUpdate = false;

		if(state == ClientStarting)
		{
			if(!manager->canWriteImmediately())
			{
				state = Stopped;
				errorCondition = ZhttpRequest::ErrorUnavailable;
				emit q->error();
				cleanup();
				return;
			}

			// even though we don't have credits yet, we can act
			//   like we do on the first packet. we'll still cap
			//   our potential size though.
			QByteArray buf = requestBodyBuf.take(IDEAL_CREDITS);

			ZhttpRequestPacket p;
			p.type = ZhttpRequestPacket::Data;
			p.method = requestMethod;
			p.uri = requestUri;
			p.headers = requestHeaders;
			p.body = buf;
			if(!requestBodyBuf.isEmpty() || !bodyFinished)
				p.more = true;
			p.stream = true;
			p.connectHost = connectHost;
			p.connectPort = connectPort;
			if(ignorePolicies)
				p.ignorePolicies = true;
			if(ignoreTlsErrors)
				p.ignoreTlsErrors = true;
			p.credits = IDEAL_CREDITS;
			writePacket(p);

			if(p.more)
				state = ClientRequestStartWait;
			else
				state = ClientRequestFinishWait;
		}
		else if(state == ClientRequesting)
		{
			tryWrite();
		}
		else if(state == ServerStarting)
		{
			if(haveRequestBody)
				state = ServerResponseWait;
			else
				state = ServerReceiving;
			emit q->readyRead();
		}
		else if(state == ServerResponseStarting)
		{
			state = ServerResponding;

			ZhttpResponsePacket packet;
			packet.code = responseCode;
			packet.reason = responseReason;
			packet.headers = responseHeaders;
			packet.body = responseBodyBuf.take(outCredits);
			outCredits -= packet.body.size();
			packet.more = (!responseBodyBuf.isEmpty() || !bodyFinished);

			writePacket(packet);

			if(!packet.more)
			{
				state = Stopped;
				cleanup();
			}

			if(!packet.body.isEmpty())
				emit q->bytesWritten(packet.body.size());
			else if(!packet.more)
				emit q->bytesWritten(0);
		}
		else if(state == ServerResponding)
		{
			tryWrite();
		}
	}

	void expire_timeout()
	{
		tryCancel();

		state = Stopped;
		errorCondition = ZhttpRequest::ErrorTimeout;
		cleanup();
		emit q->error();
	}

	void keepAlive_timeout()
	{
		if(server)
		{
			ZhttpResponsePacket p;
			p.type = ZhttpResponsePacket::KeepAlive;
			writePacket(p);
		}
		else
		{
			ZhttpRequestPacket p;
			p.type = ZhttpRequestPacket::KeepAlive;
			writePacket(p);
		}
	}
};

ZhttpRequest::ZhttpRequest(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZhttpRequest::~ZhttpRequest()
{
	delete d;
}

ZhttpRequest::Rid ZhttpRequest::rid() const
{
	return d->rid;
}

QHostAddress ZhttpRequest::peerAddress() const
{
	return d->peerAddress;
}

void ZhttpRequest::setConnectHost(const QString &host)
{
	d->connectHost = host;
}

void ZhttpRequest::setConnectPort(int port)
{
	d->connectPort = port;
}

void ZhttpRequest::setIgnorePolicies(bool on)
{
	d->ignorePolicies = on;
}

void ZhttpRequest::setIgnoreTlsErrors(bool on)
{
	d->ignoreTlsErrors = on;
}

void ZhttpRequest::start(const QString &method, const QUrl &uri, const HttpHeaders &headers)
{
	assert(!d->server);

	d->requestMethod = method;
	d->requestUri = uri;
	d->requestHeaders = headers;
	d->startClient();
}

void ZhttpRequest::beginResponse(int code, const QByteArray &reason, const HttpHeaders &headers)
{
	assert(d->server);
	assert(d->state == Private::ServerResponseWait);

	d->responseCode = code;
	d->responseReason = reason;
	d->responseHeaders = headers;
	d->beginResponse();
}

void ZhttpRequest::writeBody(const QByteArray &body)
{
	d->writeBody(body);
}

void ZhttpRequest::endBody()
{
	d->endBody();
}

void ZhttpRequest::pause()
{
	assert(d->server);
	d->pause();
}

ZhttpRequest::ServerState ZhttpRequest::serverState() const
{
	ServerState ss;
	ss.peerAddress = d->peerAddress;
	ss.requestMethod = d->requestMethod;
	ss.requestUri = d->requestUri;
	ss.requestHeaders = d->requestHeaders;
	ss.inSeq = d->inSeq;
	ss.outSeq = d->outSeq;
	ss.outCredits = d->outCredits;
	ss.userData = d->userData;
	return ss;
}

int ZhttpRequest::bytesAvailable() const
{
	if(d->server)
		return d->requestBodyBuf.size();
	else
		return d->responseBodyBuf.size();
}

bool ZhttpRequest::isFinished() const
{
	return d->state == Private::Stopped;
}

bool ZhttpRequest::isInputFinished() const
{
	if(d->server)
		return (d->state == Private::Stopped || d->state == Private::ServerResponseWait || d->state == Private::ServerResponseStarting || d->state == Private::ServerResponding);
	else
		return (d->state == Private::Stopped);
}

bool ZhttpRequest::isOutputFinished() const
{
	if(d->server)
		return (d->state == Private::Stopped);
	else
		return (d->state == Private::Stopped || d->state == Private::ClientRequestFinishWait || d->state == Private::ClientReceiving);
}

ZhttpRequest::ErrorCondition ZhttpRequest::errorCondition() const
{
	return d->errorCondition;
}

QString ZhttpRequest::requestMethod() const
{
	return d->requestMethod;
}

QUrl ZhttpRequest::requestUri() const
{
	return d->requestUri;
}

HttpHeaders ZhttpRequest::requestHeaders() const
{
	return d->requestHeaders;
}

int ZhttpRequest::responseCode() const
{
	return d->responseCode;
}

QByteArray ZhttpRequest::responseReason() const
{
	return d->responseReason;
}

HttpHeaders ZhttpRequest::responseHeaders() const
{
	return d->responseHeaders;
}

QByteArray ZhttpRequest::readBody(int size)
{
	return d->readBody(size);
}

void ZhttpRequest::setupClient(ZhttpManager *manager)
{
	d->manager = manager;
	d->rid = Rid(manager->instanceId(), QUuid::createUuid().toString().toLatin1());
	d->manager->link(this);
}

bool ZhttpRequest::setupServer(ZhttpManager *manager, const ZhttpRequestPacket &packet)
{
	d->manager = manager;
	d->server = true;
	d->rid = Rid(packet.from, packet.id);
	return d->setupServer(packet);
}

void ZhttpRequest::setupServer(ZhttpManager *manager, const ZhttpRequest::ServerState &state)
{
	d->manager = manager;
	d->server = true;
	d->rid = state.rid;
	d->manager->link(this);
	d->setupServer(state);
}

void ZhttpRequest::startServer()
{
	d->startServer();
}

bool ZhttpRequest::isServer() const
{
	return d->server;
}

void ZhttpRequest::handle(const ZhttpRequestPacket &packet)
{
	assert(d->manager);

	d->handle(packet);
}

void ZhttpRequest::handle(const ZhttpResponsePacket &packet)
{
	assert(d->manager);

	d->handle(packet);
}

#include "zhttprequest.moc"
