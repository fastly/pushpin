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

#include "zwebsocket.h"

#include <assert.h>
#include <QTimer>
#include <QPointer>
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "log.h"
#include "zhttpmanager.h"
#include "uuidutil.h"

#define IDEAL_CREDITS 200000
#define SESSION_EXPIRE 60000

class ZWebSocket::Private : public QObject
{
	Q_OBJECT

public:
	enum InternalState
	{
		Idle,
		AboutToConnect,
		Connecting,
		Connected,
		ConnectedPeerClosed,
		ClosedPeerConnected
	};

	ZWebSocket *q;
	ZhttpManager *manager;
	bool server;
	InternalState state;
	ZWebSocket::Rid rid;
	QByteArray toAddress;
	QHostAddress peerAddress;
	QString connectHost;
	int connectPort;
	bool ignorePolicies;
	bool ignoreTlsErrors;
	QUrl requestUri;
	HttpHeaders requestHeaders;
	int inSeq;
	int outSeq;
	int outCredits;
	int pendingInCredits;
	int responseCode;
	QByteArray responseReason;
	HttpHeaders responseHeaders;
	QByteArray responseBody; // for rejections only
	bool inClosed;
	bool outClosed;
	int closeCode;
	int peerCloseCode;
	QVariant userData;
	bool pendingUpdate;
	WebSocket::ErrorCondition errorCondition;
	QTimer *expireTimer;
	QTimer *keepAliveTimer;
	QList<Frame> inFrames;
	QList<Frame> outFrames;
	int inSize;
	int inContentType;
	int outContentType;

	Private(ZWebSocket *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		server(false),
		state(Idle),
		connectPort(-1),
		ignorePolicies(false),
		ignoreTlsErrors(false),
		inSeq(0),
		outSeq(0),
		outCredits(0),
		pendingInCredits(0),
		responseCode(-1),
		inClosed(false),
		outClosed(false),
		closeCode(-1),
		peerCloseCode(-1),
		pendingUpdate(false),
		expireTimer(0),
		keepAliveTimer(0),
		inSize(0),
		inContentType(-1),
		outContentType((int)Frame::Text)
	{
		expireTimer = new QTimer(this);
		connect(expireTimer, SIGNAL(timeout()), this, SLOT(expire_timeout()));
		expireTimer->setSingleShot(true);

		keepAliveTimer = new QTimer(this);
		connect(keepAliveTimer, SIGNAL(timeout()), this, SLOT(keepAlive_timeout()));
	}

	~Private()
	{
		if(manager && state != Idle)
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
			log_warning("zws server: received request with invalid type, canceling");
			tryRespondCancel(packet);
			return false;
		}

		if(packet.seq != 0)
		{
			log_warning("zws server: error, received request with non-zero seq field");
			writeError("bad-request");
			state = Idle;
			return false;
		}

		inSeq = 1; // next expected seq

		if(packet.credits != -1)
			outCredits = packet.credits;

		requestUri = packet.uri;
		requestHeaders = packet.headers;

		userData = packet.userData;
		peerAddress = packet.peerAddress;

		return true;
	}

	void startClient()
	{
		state = AboutToConnect;

		refreshTimeout();
		update();
	}

	void startServer()
	{
		state = Connecting;

		startKeepAlive();
		refreshTimeout();
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

	void respond()
	{
		state = Connected;

		ZhttpResponsePacket out;
		out.code = responseCode;
		out.reason = responseReason;
		out.headers = responseHeaders;
		writePacket(out);
	}

	void reject()
	{
		ZhttpResponsePacket out;
		out.type = ZhttpResponsePacket::Error;
		out.condition = "rejected";
		out.code = responseCode;
		out.reason = responseReason;
		out.headers = responseHeaders;
		out.body = responseBody;
		writePacket(out);

		state = Idle;
		cleanup();
		QMetaObject::invokeMethod(q, "closed", Qt::QueuedConnection);
	}

	Frame readFrame()
	{
		Frame f = inFrames.takeFirst();
		inSize -= f.data.size();
		pendingInCredits += f.data.size();
		update();
		return f;
	}

	void writeFrame(const Frame &frame)
	{
		assert(state == Connected || state == ConnectedPeerClosed);
		outFrames += frame;
		update();
	}

	void close(int code)
	{
		if((state != Connected && state != ConnectedPeerClosed) || outClosed)
			return;

		outClosed = true;
		closeCode = code;

		if(outFrames.isEmpty())
		{
			writeClose(code);

			if(state == ConnectedPeerClosed)
			{
				// if peer was already closed, then we're done!
				state = Idle;
				cleanup();
				QMetaObject::invokeMethod(q, "closed", Qt::QueuedConnection);
			}
			else
			{
				// if peer was not closed, then we wait around
				state = ClosedPeerConnected;
			}
		}
	}

	void tryWrite()
	{
		QPointer<QObject> self = this;

		if(state == Connected || state == ConnectedPeerClosed)
		{
			int written = 0;
			int contentBytesWritten = 0;

			while(!outFrames.isEmpty() && outCredits >= outFrames.first().data.size())
			{
				// if we have data to send, and the credits to do so, then send data.
				// also send credits if we need to.

				Frame f = outFrames.takeFirst();
				outCredits -= f.data.size();

				int credits = -1;
				if(state != ConnectedPeerClosed && pendingInCredits > 0)
				{
					credits = pendingInCredits;
					pendingInCredits = 0;
				}

				writeFrameInternal(f, credits);
				++written;
				contentBytesWritten += f.data.size();
			}

			if(written > 0)
			{
				emit q->framesWritten(written, contentBytesWritten);
				if(!self)
					return;
			}

			if(outFrames.isEmpty() && outClosed)
			{
				writeClose(closeCode);

				if(state == ConnectedPeerClosed)
				{
					// if peer was already closed, then we're done!
					state = Idle;
					cleanup();
					emit q->closed();
				}
				else
				{
					// if peer was not closed, then we wait around
					state = ClosedPeerConnected;
				}
			}
		}

		// if we didn't send credits in a data packet, then do them now
		if(state != ConnectedPeerClosed && pendingInCredits > 0)
		{
			int credits = pendingInCredits;
			pendingInCredits = 0;

			writeCredits(credits);
		}
	}

	void handleIncomingDataPacket(const QByteArray &contentType, const QByteArray &data, bool more)
	{
		Frame::Type ftype;
		if(inContentType != -1)
		{
			ftype = Frame::Continuation;
		}
		else
		{
			if(contentType == "binary")
				ftype = Frame::Binary;
			else
				ftype = Frame::Text;

			inContentType = (int)ftype;
		}

		inFrames += Frame(ftype, data, more);
		inSize += data.size();

		if(!more)
			inContentType = -1;
	}

	void handle(const ZhttpRequestPacket &packet)
	{
		if(packet.type == ZhttpRequestPacket::Error)
		{
			errorCondition = convertError(packet.condition);

			log_debug("zws server: error id=%s cond=%s", packet.id.data(), packet.condition.data());

			state = Idle;
			cleanup();
			emit q->error();
			return;
		}
		else if(packet.type == ZhttpRequestPacket::Cancel)
		{
			log_debug("zws server: received cancel id=%s", packet.id.data());

			errorCondition = ErrorGeneric;
			state = Idle;
			cleanup();
			emit q->error();
			return;
		}

		if(packet.seq != inSeq)
		{
			log_warning("zws server: error id=%s received message out of sequence, canceling", packet.id.data());

			tryRespondCancel(packet);

			state = Idle;
			errorCondition = ErrorGeneric;
			cleanup();
			emit q->error();
			return;
		}

		++inSeq;

		refreshTimeout();

		if(packet.type == ZhttpRequestPacket::Data)
		{
			if(inSize + packet.body.size() > IDEAL_CREDITS)
				log_warning("zws client: id=%s server is sending too fast", packet.id.data());

			handleIncomingDataPacket(packet.contentType, packet.body, packet.more);

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

			emit q->readyRead();
		}
		else if(packet.type == ZhttpRequestPacket::Close)
		{
			handlePeerClose(packet.code);
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
		else
		{
			log_debug("zws server: unsupported packet type id=%s type=%d", packet.id.data(), (int)packet.type);
		}
	}

	void handle(const ZhttpResponsePacket &packet)
	{
		if(packet.type == ZhttpResponsePacket::Error)
		{
			errorCondition = convertError(packet.condition);

			log_debug("zws client: error id=%s cond=%s", packet.id.data(), packet.condition.data());

			responseCode = packet.code;
			responseReason = packet.reason;
			responseHeaders = packet.headers;
			responseBody = packet.body;

			state = Idle;
			cleanup();
			emit q->error();
			return;
		}
		else if(packet.type == ZhttpResponsePacket::Cancel)
		{
			log_debug("zws client: received cancel id=%s", packet.id.data());

			errorCondition = ErrorGeneric;
			state = Idle;
			cleanup();
			emit q->error();
			return;
		}

		if(!packet.from.isEmpty())
			toAddress = packet.from;

		if(packet.seq != inSeq)
		{
			log_warning("zws client: error id=%s received message out of sequence, canceling", packet.id.data());

			tryRespondCancel(packet);

			state = Idle;
			errorCondition = ErrorGeneric;
			cleanup();
			emit q->error();
			return;
		}

		if(!toAddress.isEmpty() && !keepAliveTimer->isActive())
			startKeepAlive();

		++inSeq;

		refreshTimeout();

		if(packet.type == ZhttpResponsePacket::Data)
		{
			if(state == Connecting)
			{
				if(packet.from.isEmpty())
				{
					state = Idle;
					errorCondition = ErrorGeneric;
					cleanup();
					log_warning("zws client: error id=%s initial ack did not contain from field", packet.id.data());
					emit q->error();
					return;
				}

				responseCode = packet.code;
				responseReason = packet.reason;
				responseHeaders = packet.headers;

				if(packet.credits > 0)
					outCredits += packet.credits;

				state = Connected;
				update();
				emit q->connected();
			}
			else
			{
				if(inSize + packet.body.size() > IDEAL_CREDITS)
					log_warning("zws client: id=%s server is sending too fast", packet.id.data());

				handleIncomingDataPacket(packet.contentType, packet.body, packet.more);

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

				emit q->readyRead();
			}
		}
		else if(packet.type == ZhttpResponsePacket::Close)
		{
			handlePeerClose(packet.code);
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
			log_debug("zws client: unsupported packet type id=%s type=%d", packet.id.data(), (int)packet.type);
		}
	}

	void handlePeerClose(int code)
	{
		if((state == Connected || state == ClosedPeerConnected) && !inClosed)
		{
			inClosed = true;
			peerCloseCode = code;

			if(inFrames.isEmpty())
			{
				if(state == ClosedPeerConnected)
				{
					state = Idle;
					cleanup();
					emit q->closed();
				}
				else
				{
					state = ConnectedPeerClosed;
					emit q->peerClosed();
				}
			}
		}
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
			manager->writeWs(out);
		}
		else
		{
			assert(!toAddress.isEmpty());
			manager->writeWs(out, toAddress);
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
		
		manager->writeWs(out, rid.first);
	}

	void writeFrameInternal(const Frame &frame, int credits = -1)
	{
		// for content frames, set the type
		QByteArray contentType;
		if(frame.type == Frame::Binary || frame.type == Frame::Text || frame.type == Frame::Continuation)
		{
			Frame::Type ftype = (Frame::Type)-1;
			if(frame.type == Frame::Binary || frame.type == Frame::Text)
			{
				ftype = frame.type;
				outContentType = (int)frame.type;
			}
			else if(frame.type == Frame::Continuation)
			{
				ftype = (Frame::Type)outContentType;
			}

			if(ftype != (Frame::Type)-1)
			{
				if(ftype == Frame::Binary)
					contentType = "binary";
				else // Text
					contentType = "text";
			}
		}

		if(server)
		{
			ZhttpResponsePacket p;
			p.type = ZhttpResponsePacket::Data;
			p.contentType = contentType;
			p.body = frame.data;
			p.more = frame.more;
			p.credits = credits;
			writePacket(p);
		}
		else
		{
			ZhttpRequestPacket p;
			p.type = ZhttpRequestPacket::Data;
			p.contentType = contentType;
			p.body = frame.data;
			p.more = frame.more;
			p.credits = credits;
			writePacket(p);
		}
	}

	void writeCredits(int credits)
	{
		if(server)
		{
			ZhttpResponsePacket p;
			p.type = ZhttpResponsePacket::Credit;
			p.credits = credits;
			writePacket(p);
		}
		else
		{
			ZhttpRequestPacket p;
			p.type = ZhttpRequestPacket::Credit;
			p.credits = credits;
			writePacket(p);
		}
	}

	void writeClose(int code = -1)
	{
		if(server)
		{
			ZhttpResponsePacket out;
			out.type = ZhttpResponsePacket::Close;
			out.code = code;
			writePacket(out);
		}
		else
		{
			ZhttpRequestPacket out;
			out.type = ZhttpRequestPacket::Close;
			out.code = code;
			writePacket(out);
		}
	}

	void writeCancel()
	{
		if(server)
		{
			ZhttpResponsePacket out;
			out.type = ZhttpResponsePacket::Cancel;
			writePacket(out);
		}
		else
		{
			ZhttpRequestPacket out;
			out.type = ZhttpRequestPacket::Cancel;
			writePacket(out);
		}
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
		if(state != Idle)
		{
			state = Idle;

			// can't send cancel in client mode without address
			if(!server && toAddress.isEmpty())
				return;

			writeCancel();
		}
	}

	void tryRespondCancel(const ZhttpRequestPacket &packet)
	{
		// if this was not an error packet, send cancel
		if(packet.type != ZhttpRequestPacket::Error && packet.type != ZhttpRequestPacket::Cancel)
			writeCancel();
	}

	void tryRespondCancel(const ZhttpResponsePacket &packet)
	{
		// if this was not an error packet, send cancel
		if(packet.type != ZhttpResponsePacket::Error && packet.type != ZhttpResponsePacket::Cancel && !toAddress.isEmpty())
			writeCancel();
	}

	static ErrorCondition convertError(const QByteArray &cond)
	{
		// zws conditions:
		//  remote-connection-failed
		//  connection-timeout
		//  tls-error
		//  bad-request
		//  policy-violation
		//  max-size-exceeded
		//  session-timeout
		//  rejected

		if(cond == "policy-violation")
			return ErrorPolicy;
		else if(cond == "remote-connection-failed")
			return ErrorConnect;
		else if(cond == "tls-error")
			return ErrorTls;
		else if(cond == "connection-timeout")
			return ErrorConnectTimeout;
		else if(cond == "rejected")
			return ErrorRejected;
		else // lump the rest as generic
			return ErrorGeneric;
	}

public slots:
	void doUpdate()
	{
		pendingUpdate = false;

		if(server)
		{
			if(state == Connected || state == ConnectedPeerClosed)
			{
				tryWrite();
			}
		}
		else
		{
			if(state == AboutToConnect)
			{
				if(!manager->canWriteImmediately())
				{
					state = Idle;
					errorCondition = ErrorUnavailable;
					emit q->error();
					cleanup();
					return;
				}

				state = Connecting;

				ZhttpRequestPacket p;
				p.type = ZhttpRequestPacket::Data;
				p.uri = requestUri;
				p.headers = requestHeaders;
				p.connectHost = connectHost;
				p.connectPort = connectPort;
				if(ignorePolicies)
					p.ignorePolicies = true;
				if(ignoreTlsErrors)
					p.ignoreTlsErrors = true;
				p.credits = IDEAL_CREDITS;
				writePacket(p);
			}
			else if(state == Connected || state == ConnectedPeerClosed)
			{
				tryWrite();
			}
		}
	}

	void expire_timeout()
	{
		tryCancel();

		state = Idle;
		errorCondition = ErrorTimeout;
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

ZWebSocket::ZWebSocket(QObject *parent) :
	WebSocket(parent)
{
	d = new Private(this);
}

ZWebSocket::~ZWebSocket()
{
	delete d;
}

ZWebSocket::Rid ZWebSocket::rid() const
{
	return d->rid;
}

QHostAddress ZWebSocket::peerAddress() const
{
	return d->peerAddress;
}

void ZWebSocket::setConnectHost(const QString &host)
{
	d->connectHost = host;
}

void ZWebSocket::setConnectPort(int port)
{
	d->connectPort = port;
}

void ZWebSocket::setIgnorePolicies(bool on)
{
	d->ignorePolicies = on;
}

void ZWebSocket::setIgnoreTlsErrors(bool on)
{
	d->ignoreTlsErrors = on;
}

void ZWebSocket::start(const QUrl &uri, const HttpHeaders &headers)
{
	assert(!d->server);

	d->requestUri = uri;
	d->requestHeaders = headers;
	d->startClient();
}

void ZWebSocket::respondSuccess(const QByteArray &reason, const HttpHeaders &headers)
{
	assert(d->server);
	assert(d->state == Private::Connecting);

	d->responseCode = 101;
	d->responseReason = reason;
	d->responseHeaders = headers;
	d->respond();
}

void ZWebSocket::respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
{
	assert(d->server);
	assert(d->state == Private::Connecting);

	d->responseCode = code;
	d->responseReason = reason;
	d->responseHeaders = headers;
	d->responseBody = body;
	d->reject();
}

WebSocket::State ZWebSocket::state() const
{
	switch(d->state)
	{
		case Private::Idle:
			return Idle;
		case Private::AboutToConnect:
		case Private::Connecting:
			if(d->outClosed)
				return Closing;
			else
				return Connecting;
		case Private::Connected:
		case Private::ConnectedPeerClosed:
		case Private::ClosedPeerConnected:
		default:
			if(d->outClosed)
				return Closing;
			else
				return Connected;
	}
}

QUrl ZWebSocket::requestUri() const
{
	return d->requestUri;
}

HttpHeaders ZWebSocket::requestHeaders() const
{
	return d->requestHeaders;
}

int ZWebSocket::responseCode() const
{
	return d->responseCode;
}

QByteArray ZWebSocket::responseReason() const
{
	return d->responseReason;
}

HttpHeaders ZWebSocket::responseHeaders() const
{
	return d->responseHeaders;
}

QByteArray ZWebSocket::responseBody() const
{
	return d->responseBody;
}

int ZWebSocket::framesAvailable() const
{
	return d->inFrames.count();
}

bool ZWebSocket::canWrite() const
{
	return (writeBytesAvailable() > 0);
}

int ZWebSocket::writeBytesAvailable() const
{
	int avail = d->outCredits;
	foreach(const Frame &f, d->outFrames)
	{
		if(f.data.size() >= avail)
			return 0;

		avail -= f.data.size();
	}

	return avail;
}

int ZWebSocket::peerCloseCode() const
{
	return d->peerCloseCode;
}

WebSocket::ErrorCondition ZWebSocket::errorCondition() const
{
	return d->errorCondition;
}

void ZWebSocket::writeFrame(const Frame &frame)
{
	d->writeFrame(frame);
}

WebSocket::Frame ZWebSocket::readFrame()
{
	return d->readFrame();
}

void ZWebSocket::close(int code)
{
	d->close(code);
}

void ZWebSocket::setupClient(ZhttpManager *manager)
{
	d->manager = manager;
	d->rid = Rid(manager->instanceId(), UuidUtil::createUuid());
	d->manager->link(this);
}

bool ZWebSocket::setupServer(ZhttpManager *manager, const ZhttpRequestPacket &packet)
{
	d->manager = manager;
	d->server = true;
	d->rid = Rid(packet.from, packet.id);
	return d->setupServer(packet);
}

void ZWebSocket::startServer()
{
	d->startServer();
}

bool ZWebSocket::isServer() const
{
	return d->server;
}

void ZWebSocket::handle(const ZhttpRequestPacket &packet)
{
	assert(d->manager);

	d->handle(packet);
}

void ZWebSocket::handle(const ZhttpResponsePacket &packet)
{
	assert(d->manager);

	d->handle(packet);
}

#include "zwebsocket.moc"
