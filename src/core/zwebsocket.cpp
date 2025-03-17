/*
 * Copyright (C) 2014-2023 Fanout, Inc.
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

#include "zwebsocket.h"

#include <assert.h>
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "log.h"
#include "timer.h"
#include "defercall.h"
#include "zhttpmanager.h"
#include "uuidutil.h"

#define IDEAL_CREDITS 200000
#define SESSION_EXPIRE 60000
#define KEEPALIVE_INTERVAL 45000

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
	bool trustConnectHost;
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
	QString closeReason;
	int peerCloseCode;
	QString peerCloseReason;
	QVariant userData;
	bool pendingUpdate;
	bool readableChanged;
	bool writableChanged;
	ErrorCondition errorCondition;
	std::unique_ptr<Timer> expireTimer;
	std::unique_ptr<Timer> keepAliveTimer;
	QList<Frame> inFrames;
	QList<Frame> outFrames;
	int inSize;
	int outSize;
	int inContentType;
	int outContentType;
	bool multi;
	DeferCall deferCall;

	Private(ZWebSocket *_q) :
		q(_q),
		manager(0),
		server(false),
		state(Idle),
		connectPort(-1),
		ignorePolicies(false),
		trustConnectHost(false),
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
		readableChanged(false),
		writableChanged(false),
		inSize(0),
		outSize(0),
		inContentType(-1),
		outContentType((int)Frame::Text),
		multi(false)
	{
		expireTimer = std::make_unique<Timer>();
		expireTimer->timeout.connect(boost::bind(&Private::expire_timeout, this));
		expireTimer->setSingleShot(true);

		keepAliveTimer = std::make_unique<Timer>();
		keepAliveTimer->timeout.connect(boost::bind(&Private::keepAlive_timeout, this));
	}

	~Private()
	{
		if(manager && state != Idle)
			tryCancel();

		cleanup();
	}

	void cleanup()
	{
		readableChanged = false;
		writableChanged = false;

		expireTimer.reset();
		keepAliveTimer.reset();

		if(manager)
		{
			manager->unregisterKeepAlive(q);

			manager->unlink(q);
			manager = 0;
		}
	}

	bool setupServer(int seq, const ZhttpRequestPacket &packet)
	{
		if(packet.type != ZhttpRequestPacket::Data)
		{
			log_warning("zws server: received request with invalid type, canceling");
			tryRespondCancel(packet);
			return false;
		}

		if(seq != 0)
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

		if(packet.multi)
			multi = true;

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
		if(multi)
		{
			if(keepAliveTimer->isActive())
			{
				// need to flush the current keepalive, since the
				//   manager registration may extend the timeout
				keepAlive_timeout();

				keepAliveTimer->stop();
			}

			manager->registerKeepAlive(q);
		}
		else
		{
			manager->unregisterKeepAlive(q);

			if(!keepAliveTimer->isActive())
				keepAliveTimer->start(KEEPALIVE_INTERVAL);
		}
	}

	void stopKeepAlive()
	{
		if(keepAliveTimer->isActive())
			keepAliveTimer->stop();

		manager->unregisterKeepAlive(q);
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
			deferCall.defer([=] { doUpdate(); });
		}
	}

	void respond()
	{
		state = Connected;

		ZhttpResponsePacket out;
		out.type = ZhttpResponsePacket::Data;
		out.code = responseCode;
		out.reason = responseReason;
		out.headers = responseHeaders;
		out.credits = IDEAL_CREDITS;
		if(multi)
			out.multi = true;
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
		deferCall.defer([=] { doClosed(); });
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
		if(state != Connected && state != ConnectedPeerClosed)
			return;

		outFrames += frame;
		outSize += frame.data.size();
		update();
	}

	void close(int code, const QString &reason)
	{
		if((state != Connected && state != ConnectedPeerClosed) || outClosed)
			return;

		outClosed = true;
		closeCode = code;
		closeReason = reason;

		if(outFrames.isEmpty())
		{
			writeClose(code, reason);

			if(state == ConnectedPeerClosed)
			{
				// if peer was already closed, then we're done!
				state = Idle;
				cleanup();
				deferCall.defer([=] { doClosed(); });
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
		std::weak_ptr<Private> self = q->d;

		if(state == Connected || state == ConnectedPeerClosed)
		{
			int written = 0;
			int contentBytesWritten = 0;

			while(!outFrames.isEmpty())
			{
				Frame &nextFrame = outFrames.first();
				int contentSize = 0;

				if((nextFrame.type == Frame::Ping || nextFrame.type == Frame::Pong) && outCredits >= nextFrame.data.size())
				{
					contentSize = nextFrame.data.size();
				}
				else if((nextFrame.type == Frame::Text || nextFrame.type == Frame::Binary || nextFrame.type == Frame::Continuation) && (nextFrame.data.isEmpty() || outCredits > 0))
				{
					contentSize = qMin(nextFrame.data.size(), outCredits);
				}
				else
				{
					break;
				}

				// if we have data to send, and the credits to do so, then send data.
				// also send credits if we need to.

				Frame f = nextFrame;
				bool outFrameDone = false;

				if(contentSize >= nextFrame.data.size())
				{
					outFrames.removeFirst();
					outFrameDone = true;
				}
				else
				{
					f.data = f.data.mid(0, contentSize);
					f.more = true;

					nextFrame.type = Frame::Continuation;
					nextFrame.data = nextFrame.data.mid(contentSize);
				}

				outSize -= f.data.size();
				outCredits -= f.data.size();

				int credits = -1;
				if(state != ConnectedPeerClosed && pendingInCredits > 0)
				{
					credits = pendingInCredits;
					pendingInCredits = 0;
				}

				writeFrameInternal(f, credits);

				if(outFrameDone)
					++written;

				contentBytesWritten += f.data.size();
			}

			if(written > 0 || contentBytesWritten > 0)
			{
				q->framesWritten(written, contentBytesWritten);
				if(self.expired())
					return;
			}

			if(outFrames.isEmpty() && outClosed)
			{
				writeClose(closeCode, closeReason);

				if(state == ConnectedPeerClosed)
				{
					// if peer was already closed, then we're done!
					state = Idle;
					cleanup();
					q->closed();
					return;
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

		inFrames += Frame(ftype, !data.isNull() ? data : QByteArray(""), more);
		inSize += data.size();

		if(!more)
			inContentType = -1;
	}

	void handle(const QByteArray &id, int seq, const ZhttpRequestPacket &packet)
	{
		if(packet.type == ZhttpRequestPacket::Error)
		{
			errorCondition = convertError(packet.condition);

			log_debug("zws server: error id=%s cond=%s", id.data(), packet.condition.data());

			state = Idle;
			cleanup();
			q->error();
			return;
		}
		else if(packet.type == ZhttpRequestPacket::Cancel)
		{
			log_debug("zws server: received cancel id=%s", id.data());

			errorCondition = ErrorGeneric;
			state = Idle;
			cleanup();
			q->error();
			return;
		}

		if(seq != inSeq)
		{
			log_warning("zws server: error id=%s received message out of sequence, canceling", id.data());

			tryRespondCancel(packet);

			state = Idle;
			errorCondition = ErrorGeneric;
			cleanup();
			q->error();
			return;
		}

		++inSeq;

		if(!multi && packet.multi)
		{
			// switch on multi support
			multi = true;
			startKeepAlive(); // re-setup keep alive
		}

		refreshTimeout();

		if(packet.type == ZhttpRequestPacket::Data || packet.type == ZhttpRequestPacket::Ping || packet.type == ZhttpRequestPacket::Pong)
		{
			if(inSize + packet.body.size() > IDEAL_CREDITS)
				log_warning("zws client: id=%s server is sending too fast", id.data());

			if(packet.type == ZhttpRequestPacket::Data)
			{
				handleIncomingDataPacket(packet.contentType, packet.body, packet.more);
			}
			else if(packet.type == ZhttpRequestPacket::Ping)
			{
				inFrames += Frame(Frame::Ping, packet.body, false);
				inSize += packet.body.size();
			}
			else if(packet.type == ZhttpRequestPacket::Pong)
			{
				inFrames += Frame(Frame::Pong, packet.body, false);
				inSize += packet.body.size();
			}

			if(packet.credits > 0)
			{
				outCredits += packet.credits;
				writableChanged = true;
			}

			readableChanged = true;
			update();
		}
		else if(packet.type == ZhttpRequestPacket::Close)
		{
			handlePeerClose(packet.code, QString::fromUtf8(packet.body));
		}
		else if(packet.type == ZhttpRequestPacket::Credit)
		{
			if(packet.credits > 0)
			{
				outCredits += packet.credits;
				writableChanged = true;
				update();
			}
		}
		else if(packet.type == ZhttpRequestPacket::KeepAlive)
		{
			// nothing to do
		}
		else
		{
			log_debug("zws server: unsupported packet type id=%s type=%d", id.data(), (int)packet.type);
		}
	}

	void handle(const QByteArray &id, int seq, const ZhttpResponsePacket &packet)
	{
		if(packet.type == ZhttpResponsePacket::Error)
		{
			errorCondition = convertError(packet.condition);

			log_debug("zws client: error id=%s cond=%s", id.data(), packet.condition.data());

			responseCode = packet.code;
			responseReason = packet.reason;
			responseHeaders = packet.headers;
			responseBody = packet.body;

			state = Idle;
			cleanup();
			q->error();
			return;
		}
		else if(packet.type == ZhttpResponsePacket::Cancel)
		{
			log_debug("zws client: received cancel id=%s", id.data());

			errorCondition = ErrorGeneric;
			state = Idle;
			cleanup();
			q->error();
			return;
		}

		if(!packet.from.isEmpty())
			toAddress = packet.from;

		if(seq != inSeq)
		{
			log_warning("zws client: error id=%s received message out of sequence, canceling", id.data());

			tryRespondCancel(packet);

			state = Idle;
			errorCondition = ErrorGeneric;
			cleanup();
			q->error();
			return;
		}

		if(!toAddress.isEmpty())
			startKeepAlive(); // only starts if wasn't started already

		++inSeq;

		if(!multi && packet.multi)
		{
			// switch on multi support
			multi = true;
			startKeepAlive(); // re-setup keep alive
		}

		refreshTimeout();

		if(state == Connecting)
		{
			if(packet.type != ZhttpResponsePacket::Data && packet.type != ZhttpResponsePacket::Credit && packet.type != ZhttpResponsePacket::KeepAlive)
			{
				state = Idle;
				errorCondition = ErrorGeneric;
				cleanup();
				log_warning("zws client: error id=%s initial response wrong type", id.data());
				q->error();
				return;
			}

			if(packet.from.isEmpty())
			{
				state = Idle;
				errorCondition = ErrorGeneric;
				cleanup();
				log_warning("zws client: error id=%s initial ack did not contain from field", id.data());
				q->error();
				return;
			}
		}

		if(packet.type == ZhttpResponsePacket::Data || packet.type == ZhttpResponsePacket::Ping || packet.type == ZhttpResponsePacket::Pong)
		{
			if(state == Connecting)
			{
				// this is assured earlier
				assert(packet.type == ZhttpResponsePacket::Data);

				responseCode = packet.code;
				responseReason = packet.reason;
				responseHeaders = packet.headers;

				if(packet.credits > 0)
					outCredits += packet.credits;

				state = Connected;
				update();
				q->connected();
			}
			else
			{
				if(inSize + packet.body.size() > IDEAL_CREDITS)
					log_warning("zws client: id=%s server is sending too fast", id.data());

				if(packet.type == ZhttpResponsePacket::Data)
				{
					handleIncomingDataPacket(packet.contentType, packet.body, packet.more);
				}
				else if(packet.type == ZhttpResponsePacket::Ping)
				{
					inFrames += Frame(Frame::Ping, packet.body, false);
					inSize += packet.body.size();
				}
				else if(packet.type == ZhttpResponsePacket::Pong)
				{
					inFrames += Frame(Frame::Pong, packet.body, false);
					inSize += packet.body.size();
				}

				if(packet.credits > 0)
				{
					outCredits += packet.credits;
					writableChanged = true;
				}

				readableChanged = true;
				update();
			}
		}
		else if(packet.type == ZhttpResponsePacket::Close)
		{
			handlePeerClose(packet.code, QString::fromUtf8(packet.body));
		}
		else if(packet.type == ZhttpResponsePacket::Credit)
		{
			if(packet.credits > 0)
			{
				outCredits += packet.credits;
				writableChanged = true;
				update();
			}
		}
		else if(packet.type == ZhttpResponsePacket::KeepAlive)
		{
			// nothing to do
		}
		else
		{
			log_debug("zws client: unsupported packet type id=%s type=%d", id.data(), (int)packet.type);
		}
	}

	void handlePeerClose(int code, const QString &reason)
	{
		if((state == Connected || state == ClosedPeerConnected) && !inClosed)
		{
			inClosed = true;
			peerCloseCode = code;
			peerCloseReason = reason;

			if(inFrames.isEmpty())
			{
				if(state == ClosedPeerConnected)
				{
					state = Idle;
					cleanup();
					q->closed();
				}
				else
				{
					state = ConnectedPeerClosed;
					q->peerClosed();
				}
			}
		}
	}

	void writePacket(const ZhttpRequestPacket &packet)
	{
		assert(manager);

		bool first = (outSeq == 0);

		ZhttpRequestPacket out = packet;
		out.from = rid.first;
		out.ids += ZhttpRequestPacket::Id(rid.second, outSeq++);

		if(first)
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
		out.ids += ZhttpResponsePacket::Id(rid.second, outSeq++);
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

			if(frame.type == Frame::Ping)
			{
				p.type = ZhttpResponsePacket::Ping;
			}
			else if(frame.type == Frame::Pong)
			{
				p.type = ZhttpResponsePacket::Pong;
			}
			else
			{
				p.type = ZhttpResponsePacket::Data;
				p.contentType = contentType;
			}

			p.body = frame.data;
			p.more = frame.more;
			p.credits = credits;
			writePacket(p);
		}
		else
		{
			ZhttpRequestPacket p;

			if(frame.type == Frame::Ping)
			{
				p.type = ZhttpRequestPacket::Ping;
			}
			else if(frame.type == Frame::Pong)
			{
				p.type = ZhttpRequestPacket::Pong;
			}
			else
			{
				p.type = ZhttpRequestPacket::Data;
				p.contentType = contentType;
			}

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

	void writeClose(int code = -1, const QString &reason = QString())
	{
		if(server)
		{
			ZhttpResponsePacket out;
			out.type = ZhttpResponsePacket::Close;
			out.code = code;
			if(!reason.isEmpty())
				out.body = reason.toUtf8();
			writePacket(out);
		}
		else
		{
			ZhttpRequestPacket out;
			out.type = ZhttpRequestPacket::Close;
			out.code = code;
			if(!reason.isEmpty())
				out.body = reason.toUtf8();
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

	void doClosed()
	{
		q->closed();
	}

	void doUpdate()
	{
		pendingUpdate = false;

		if(state == Connected || state == ClosedPeerConnected)
		{
			if(inFrames.isEmpty() && inClosed)
			{
				if(state == ClosedPeerConnected)
				{
					state = Idle;
					cleanup();
					q->closed();
					return;
				}
				else
				{
					std::weak_ptr<Private> self = q->d;
					state = ConnectedPeerClosed;
					q->peerClosed();
					if(self.expired())
						return;
				}
			}
			else
			{
				if(readableChanged)
				{
					readableChanged = false;

					std::weak_ptr<Private> self = q->d;
					q->readyRead();
					if(self.expired())
						return;
				}
			}
		}

		if(state == AboutToConnect)
		{
			if(!manager->canWriteImmediately())
			{
				state = Idle;
				errorCondition = ErrorUnavailable;
				cleanup();
				q->error();
				return;
			}

			state = Connecting;

			ZhttpRequestPacket p;
			p.type = ZhttpRequestPacket::Data;
			p.uri = requestUri;
			p.headers = requestHeaders;
			p.routerResp = true;
			p.connectHost = connectHost;
			p.connectPort = connectPort;
			if(ignorePolicies)
				p.ignorePolicies = true;
			if(trustConnectHost)
				p.trustConnectHost = true;
			if(ignoreTlsErrors)
				p.ignoreTlsErrors = true;
			p.credits = IDEAL_CREDITS;
			p.multi = true;
			writePacket(p);
		}
		else if(state == Connected || state == ConnectedPeerClosed)
		{
			std::weak_ptr<Private> self = q->d;
			tryWrite();
			if(self.expired())
				return;

			if(writableChanged)
			{
				writableChanged = false;

				q->writeBytesChanged();
			}
		}
	}

	void expire_timeout()
	{
		state = Idle;
		errorCondition = ErrorTimeout;
		cleanup();
		q->error();
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

ZWebSocket::ZWebSocket()
{
	d = std::make_shared<Private>(this);
}

ZWebSocket::~ZWebSocket() = default;

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

void ZWebSocket::setTrustConnectHost(bool on)
{
	d->trustConnectHost = on;
}

void ZWebSocket::setIgnoreTlsErrors(bool on)
{
	d->ignoreTlsErrors = on;
}

void ZWebSocket::setIsTls(bool on)
{
	d->requestUri.setScheme(on ? "wss" : "ws");
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

int ZWebSocket::writeBytesAvailable() const
{
	if(d->outSize < d->outCredits)
		return d->outCredits - d->outSize;
	else
		return 0;
}

int ZWebSocket::peerCloseCode() const
{
	return d->peerCloseCode;
}

QString ZWebSocket::peerCloseReason() const
{
	return d->peerCloseReason;
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

void ZWebSocket::close(int code, const QString &reason)
{
	d->close(code, reason);
}

void ZWebSocket::setupClient(ZhttpManager *manager)
{
	d->manager = manager;
	d->rid = Rid(manager->instanceId(), UuidUtil::createUuid());
	d->manager->link(this);
}

bool ZWebSocket::setupServer(ZhttpManager *manager, const QByteArray &id, int seq, const ZhttpRequestPacket &packet)
{
	d->manager = manager;
	d->server = true;
	d->rid = Rid(packet.from, id);
	return d->setupServer(seq, packet);
}

void ZWebSocket::startServer()
{
	d->startServer();
}

bool ZWebSocket::isServer() const
{
	return d->server;
}

QByteArray ZWebSocket::toAddress() const
{
	return d->toAddress;
}

int ZWebSocket::outSeqInc()
{
	return d->outSeq++;
}

void ZWebSocket::handle(const QByteArray &id, int seq, const ZhttpRequestPacket &packet)
{
	assert(d->manager);

	d->handle(id, seq, packet);
}

void ZWebSocket::handle(const QByteArray &id, int seq, const ZhttpResponsePacket &packet)
{
	assert(d->manager);

	d->handle(id, seq, packet);
}

#include "zwebsocket.moc"
