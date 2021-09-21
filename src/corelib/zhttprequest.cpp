/*
 * Copyright (C) 2012-2021 Fanout, Inc.
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

#include "zhttprequest.h"

#include <assert.h>
#include <QPointer>
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "bufferlist.h"
#include "log.h"
#include "rtimer.h"
#include "zhttpmanager.h"
#include "uuidutil.h"

#define IDEAL_CREDITS 200000
#define SESSION_EXPIRE 60000
#define KEEPALIVE_INTERVAL 45000
#define REQ_BUF_MAX 1000000

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
	bool doReq;
	QByteArray toAddress;
	QHostAddress peerAddress;
	QString connectHost;
	int connectPort;
	bool ignorePolicies;
	bool trustConnectHost;
	bool ignoreTlsErrors;
	bool sendBodyAfterAck;
	QVariant passthrough;
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
	bool needPause;
	bool errored;
	ErrorCondition errorCondition;
	RTimer *expireTimer;
	RTimer *keepAliveTimer;
	bool multi;
	bool quiet;

	Private(ZhttpRequest *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		server(false),
		state(Stopped),
		doReq(false),
		connectPort(-1),
		ignorePolicies(false),
		trustConnectHost(false),
		ignoreTlsErrors(false),
		sendBodyAfterAck(false),
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
		needPause(false),
		errored(false),
		expireTimer(0),
		keepAliveTimer(0),
		multi(false),
		quiet(false)
	{
		expireTimer = new RTimer(this);
		connect(expireTimer, &RTimer::timeout, this, &Private::expire_timeout);
		expireTimer->setSingleShot(true);

		keepAliveTimer = new RTimer(this);
		connect(keepAliveTimer, &RTimer::timeout, this, &Private::keepAlive_timeout);
	}

	~Private()
	{
		if(manager && !paused && state != Stopped)
			tryCancel();

		cleanup();
	}

	void cleanup()
	{
		needPause = false;

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
			manager->unregisterKeepAlive(q);

			manager->unlink(q);
			manager = 0;
		}
	}

	bool setupServer(int seq, const ZhttpRequestPacket &packet)
	{
		if(packet.type != ZhttpRequestPacket::Data)
		{
			log_warning("zhttp server: received request with invalid type, canceling");
			tryRespondCancel(packet);
			return false;
		}

		if(seq != -1 && seq != 0)
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

		if(seq == -1 && packet.more)
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

		passthrough = packet.passthrough;

		userData = packet.userData;
		peerAddress = packet.peerAddress;

		if(packet.multi)
			multi = true;

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
		requestBodyBuf += ss.requestBody;
		if(ss.inSeq >= 0)
			inSeq = ss.inSeq;
		if(ss.outSeq >= 0)
			outSeq = ss.outSeq;
		if(ss.outCredits >= 0)
			outCredits = ss.outCredits;
		userData = ss.userData;

		if(ss.responseCode != -1)
		{
			responseCode = ss.responseCode;
			state = ServerResponding;
		}
		else
		{
			state = ServerResponseWait;
		}

		refreshTimeout();
		startKeepAlive();

		// send a keep-alive right away to accept after handoff
		ZhttpResponsePacket p;
		p.type = ZhttpResponsePacket::KeepAlive;
		p.multi = true; // request multi support
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
		assert(!pausing && !paused);
		assert(!doReq);

		stopKeepAlive();

		pausing = true;
		needPause = true;

		update();
	}

	void resume()
	{
		assert(paused);
		paused = false;

		startKeepAlive();

		ZhttpResponsePacket p;
		p.type = ZhttpResponsePacket::KeepAlive;
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
		QPointer<QObject> self = this;

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
				packet.type = ZhttpResponsePacket::Data;
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

		if(!self)
			return;

		trySendPause();
	}

	void trySendPause()
	{
		if(needPause && (state == ServerResponseWait || state == ServerResponding) && responseBodyBuf.isEmpty())
		{
			needPause = false;

			ZhttpResponsePacket p;
			p.type = ZhttpResponsePacket::HandoffStart;
			writePacket(p);
		}
	}

	void handle(const QByteArray &id, int seq, const ZhttpRequestPacket &packet)
	{
		if(paused)
			return;

		if(packet.type == ZhttpRequestPacket::Error)
		{
			errored = true;
			errorCondition = convertError(packet.condition);

			log_debug("zhttp server: error id=%s cond=%s", id.data(), packet.condition.data());

			state = Stopped;
			cleanup();
			emit q->error();
			return;
		}
		else if(packet.type == ZhttpRequestPacket::Cancel)
		{
			log_debug("zhttp server: received cancel id=%s", id.data());

			errored = true;
			errorCondition = ErrorGeneric;
			state = Stopped;
			cleanup();
			emit q->error();
			return;
		}

		if(seq != -1)
		{
			if(seq != inSeq)
			{
				log_warning("zhttp server: error id=%s received message out of sequence, canceling", id.data());

				// if this was not an error packet, send cancel
				if(packet.type != ZhttpRequestPacket::Error && packet.type != ZhttpRequestPacket::Cancel)
				{
					ZhttpResponsePacket p;
					p.type = ZhttpResponsePacket::Cancel;
					writePacket(p);
				}

				state = Stopped;
				errored = true;
				errorCondition = ErrorGeneric;
				cleanup();
				emit q->error();
				return;
			}

			++inSeq;
		}

		if(!multi && packet.multi)
		{
			// switch on multi support
			multi = true;

			if(!pausing)
			{
				// re-setup keep alive
				startKeepAlive();
			}
		}

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
			log_debug("zhttp server: unsupported packet type id=%s type=%d", id.data(), (int)packet.type);
		}
	}

	void handle(const QByteArray &id, int seq, const ZhttpResponsePacket &packet)
	{
		if(state == ClientRequestStartWait)
		{
			if(packet.from.isEmpty())
			{
				state = Stopped;
				errored = true;
				errorCondition = ErrorGeneric;
				cleanup();
				log_warning("zhttp client: error id=%s initial ack for streamed input request did not contain from field", id.data());
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

			if(!doReq)
				startKeepAlive();
		}

		if(packet.type == ZhttpResponsePacket::Error)
		{
			errored = true;
			errorCondition = convertError(packet.condition);

			log_debug("zhttp client: error id=%s cond=%s", id.data(), packet.condition.data());

			state = Stopped;
			cleanup();
			emit q->error();
			return;
		}
		else if(packet.type == ZhttpResponsePacket::Cancel)
		{
			log_debug("zhttp client: received cancel id=%s", id.data());

			errored = true;
			errorCondition = ErrorGeneric;
			state = Stopped;
			cleanup();
			emit q->error();
			return;
		}

		// if non-req mode, check sequencing
		if(!doReq && seq != inSeq)
		{
			log_warning("zhttp client: error id=%s received message out of sequence, canceling", id.data());

			// if this was not an error packet, send cancel
			if(packet.type != ZhttpResponsePacket::Error && packet.type != ZhttpResponsePacket::Cancel)
			{
				ZhttpRequestPacket p;
				p.type = ZhttpRequestPacket::Cancel;
				writePacket(p);
			}

			state = Stopped;
			errored = true;
			errorCondition = ErrorGeneric;
			cleanup();
			emit q->error();
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

		if(doReq && (packet.type != ZhttpResponsePacket::Data || packet.more))
		{
			log_warning("zhttp/zws client req: received invalid req response");

			state = Stopped;
			errored = true;
			errorCondition = ErrorGeneric;
			cleanup();
			emit q->error();
			return;
		}

		if(packet.type == ZhttpResponsePacket::Data)
		{
			bool needToSendHeaders = false;

			if(!haveResponseValues)
			{
				haveResponseValues = true;

				responseCode = packet.code;
				responseReason = packet.reason;
				responseHeaders = packet.headers;

				needToSendHeaders = true;
			}

			if(doReq)
			{
				if(responseBodyBuf.size() + packet.body.size() > REQ_BUF_MAX)
					log_warning("zhttp client req: id=%s server response too large", id.data());
			}
			else
			{
				if(responseBodyBuf.size() + packet.body.size() > IDEAL_CREDITS)
					log_warning("zhttp client: id=%s server is sending too fast", id.data());
			}

			responseBodyBuf += packet.body;

			if(!doReq && packet.credits > 0)
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
				if(needToSendHeaders || !packet.body.isEmpty())
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
			log_debug("zhttp client: unsupported packet type id=%s type=%d", id.data(), (int)packet.type);
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

		if(doReq)
		{
			out.ids += ZhttpRequestPacket::Id(rid.second);

			manager->writeHttp(out);
		}
		else
		{
			bool first = (outSeq == 0);

			out.ids += ZhttpRequestPacket::Id(rid.second, outSeq++);

			if(first)
			{
				manager->writeHttp(out);
			}
			else
			{
				assert(!toAddress.isEmpty());
				manager->writeHttp(out, toAddress);
			}
		}
	}

	void writePacket(const ZhttpResponsePacket &packet)
	{
		assert(manager);

		ZhttpResponsePacket out = packet;
		out.from = manager->instanceId();
		out.ids += ZhttpResponsePacket::Id(rid.second, outSeq++);
		out.userData = userData;
		
		manager->writeHttp(out, rid.first);
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

			if(!doReq)
			{
				ZhttpRequestPacket p;
				p.type = ZhttpRequestPacket::Cancel;
				writePacket(p);
			}
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
		else if(cond == "disconnected")
			return ErrorDisconnected;
		else // lump the rest as generic
			return ErrorGeneric;
	}

public slots:
	void doUpdate()
	{
		pendingUpdate = false;

		if(state == ClientStarting)
		{
			if(doReq)
			{
				if(requestBodyBuf.size() > REQ_BUF_MAX)
				{
					state = Stopped;
					errored = true;
					errorCondition = ErrorRequestTooLarge;
					cleanup();
					emit q->error();
					return;
				}

				// for req mode, wait until request is fully supplied then send in one packet
				if(bodyFinished)
				{
					ZhttpRequestPacket p;
					p.type = ZhttpRequestPacket::Data;
					p.method = requestMethod;
					p.uri = requestUri;
					p.headers = requestHeaders;
					p.body = requestBodyBuf.take();
					p.maxSize = REQ_BUF_MAX;
					p.connectHost = connectHost;
					p.connectPort = connectPort;
					if(ignorePolicies)
						p.ignorePolicies = true;
					if(trustConnectHost)
						p.trustConnectHost = true;
					if(ignoreTlsErrors)
						p.ignoreTlsErrors = true;
					if(passthrough.isValid())
						p.passthrough = passthrough;
					if(quiet)
						p.quiet = true;
					writePacket(p);

					state = ClientRequestFinishWait;

					emit q->bytesWritten(p.body.size());
				}
			}
			else
			{
				// NOTE: not quite sure why we do this. maybe to avoid a
				//   zhttp PUSH/SUB race?
				if(!manager->canWriteImmediately())
				{
					state = Stopped;
					errored = true;
					errorCondition = ErrorUnavailable;
					cleanup();
					emit q->error();
					return;
				}

				ZhttpRequestPacket p;
				p.type = ZhttpRequestPacket::Data;
				p.method = requestMethod;
				p.uri = requestUri;
				p.headers = requestHeaders;

				if(!sendBodyAfterAck)
				{
					// even though we don't have credits yet, we can act
					//   like we do on the first packet. we'll still cap
					//   our potential size though.
					p.body = requestBodyBuf.take(IDEAL_CREDITS);
				}

				if(!requestBodyBuf.isEmpty() || !bodyFinished)
					p.more = true;
				p.stream = true;
				p.connectHost = connectHost;
				p.connectPort = connectPort;
				if(ignorePolicies)
					p.ignorePolicies = true;
				if(trustConnectHost)
					p.trustConnectHost = true;
				if(ignoreTlsErrors)
					p.ignoreTlsErrors = true;
				if(passthrough.isValid())
					p.passthrough = passthrough;
				if(quiet)
					p.quiet = true;
				p.credits = IDEAL_CREDITS;
				p.multi = true;
				writePacket(p);

				if(p.more)
					state = ClientRequestStartWait;
				else
					state = ClientRequestFinishWait;

				if(!p.body.isEmpty())
					emit q->bytesWritten(p.body.size());
				else if(!p.more)
					emit q->bytesWritten(0);
			}
		}
		else if(state == ClientRequesting)
		{
			tryWrite();
		}
		else if(state == ServerStarting)
		{
			if(haveRequestBody)
			{
				state = ServerResponseWait;

				// send ack
				ZhttpResponsePacket p;
				p.type = ZhttpResponsePacket::KeepAlive;
				if(multi)
					p.multi = true;
				writePacket(p);
			}
			else
			{
				state = ServerReceiving;

				// send credits ack
				ZhttpResponsePacket p;
				p.type = ZhttpResponsePacket::Credit;
				p.credits = IDEAL_CREDITS - responseBodyBuf.size();
				if(multi)
					p.multi = true;
				writePacket(p);
			}

			emit q->readyRead();
		}
		else if(state == ServerResponseWait)
		{
			trySendPause();
		}
		else if(state == ServerResponseStarting)
		{
			state = ServerResponding;

			ZhttpResponsePacket packet;
			packet.type = ZhttpResponsePacket::Data;
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

			QPointer<QObject> self = this;

			if(!packet.body.isEmpty())
				emit q->bytesWritten(packet.body.size());
			else if(!packet.more)
				emit q->bytesWritten(0);

			if(!self)
				return;

			trySendPause();
		}
		else if(state == ServerResponding)
		{
			tryWrite();
		}
	}

	void expire_timeout()
	{
		state = Stopped;
		errored = true;
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

ZhttpRequest::ZhttpRequest(QObject *parent) :
	HttpRequest(parent)
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

QVariant ZhttpRequest::passthroughData() const
{
	return d->passthrough;
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

void ZhttpRequest::setTrustConnectHost(bool on)
{
	d->trustConnectHost = on;
}

void ZhttpRequest::setIgnoreTlsErrors(bool on)
{
	d->ignoreTlsErrors = on;
}

void ZhttpRequest::setIsTls(bool on)
{
	d->requestUri.setScheme(on ? "https" : "http");
}

void ZhttpRequest::setSendBodyAfterAcknowledgement(bool on)
{
	d->sendBodyAfterAck = on;
}

void ZhttpRequest::setPassthroughData(const QVariant &data)
{
	d->passthrough = data;
}

void ZhttpRequest::setQuiet(bool on)
{
	d->quiet = on;
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
	assert(d->state == Private::ServerReceiving || d->state == Private::ServerResponseWait);

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

void ZhttpRequest::resume()
{
	assert(d->server);
	d->resume();
}

ZhttpRequest::ServerState ZhttpRequest::serverState() const
{
	ServerState ss;
	ss.peerAddress = d->peerAddress;
	ss.requestMethod = d->requestMethod;
	ss.requestUri = d->requestUri;
	ss.requestHeaders = d->requestHeaders;
	if(d->state == Private::ServerResponding)
		ss.responseCode = d->responseCode;
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

int ZhttpRequest::writeBytesAvailable() const
{
	if(d->responseBodyBuf.size() <= IDEAL_CREDITS)
		return (IDEAL_CREDITS - d->responseBodyBuf.size());
	else
		return 0;
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

bool ZhttpRequest::isErrored() const
{
	return d->errored;
}

HttpRequest::ErrorCondition ZhttpRequest::errorCondition() const
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

void ZhttpRequest::setupClient(ZhttpManager *manager, bool req)
{
	d->manager = manager;
	d->rid = Rid(manager->instanceId(), UuidUtil::createUuid());
	d->doReq = req;
	d->manager->link(this);
}

bool ZhttpRequest::setupServer(ZhttpManager *manager, const QByteArray &id, int seq, const ZhttpRequestPacket &packet)
{
	d->manager = manager;
	d->server = true;
	d->rid = Rid(packet.from, id);
	return d->setupServer(seq, packet);
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

QByteArray ZhttpRequest::toAddress() const
{
	return d->toAddress;
}

int ZhttpRequest::outSeqInc()
{
	return d->outSeq++;
}

void ZhttpRequest::handle(const QByteArray &id, int seq, const ZhttpRequestPacket &packet)
{
	assert(d->manager);

	d->handle(id, seq, packet);
}

void ZhttpRequest::handle(const QByteArray &id, int seq, const ZhttpResponsePacket &packet)
{
	assert(d->manager);

	d->handle(id, seq, packet);
}

#include "zhttprequest.moc"
