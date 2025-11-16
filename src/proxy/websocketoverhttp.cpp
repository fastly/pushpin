/*
 * Copyright (C) 2014-2022 Fanout, Inc.
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

#include "websocketoverhttp.h"

#include <assert.h>
#include <QRandomGenerator>
#include "log.h"
#include "bufferlist.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "zhttprequest.h"
#include "zhttpmanager.h"
#include "uuidutil.h"
#include "timer.h"
#include "defercall.h"

#define BUFFER_SIZE 200000
#define FRAME_SIZE_MAX 16384
#define RESPONSE_BODY_MAX 1000000
#define REJECT_BODY_MAX 100000
#define RETRY_TIMEOUT 1000
#define RETRY_MAX 5
#define RETRY_RAND_MAX 1000

class WebSocketOverHttp::DisconnectManager
{
public:
	~DisconnectManager()
	{
		while(!wsConnectionMap.empty())
		{
			auto it = wsConnectionMap.begin();
			WebSocketOverHttp *sock = it->first;
			wsConnectionMap.erase(it);
			deleteSocket(sock);
		}
	}

	void addSocket(WebSocketOverHttp *sock)
	{
		wsConnectionMap[sock] = { 
			sock->disconnected.connect(boost::bind(&DisconnectManager::sock_disconnected, this, sock)),
			sock->closed.connect(boost::bind(&DisconnectManager::sock_closed, this, sock)),
			sock->error.connect(boost::bind(&DisconnectManager::sock_error, this, sock))
		};

		sock->sendDisconnect();
	}

	int count() const
	{
		return wsConnectionMap.size();
	}

	bool contains(WebSocketOverHttp *sock)
	{
		auto it = wsConnectionMap.find(sock);
		return (it != wsConnectionMap.end());
	}

private:
	struct WSConnections {
		Connection disconnectedConnection;
		Connection closedConnection;
		Connection errorConnection;
	};

	map<WebSocketOverHttp *, WSConnections> wsConnectionMap;

	void deleteSocket(WebSocketOverHttp *sock);

	void cleanupSocket(WebSocketOverHttp *sock)
	{
		wsConnectionMap.erase(sock);
		deleteSocket(sock);
	}

	void sock_disconnected(WebSocketOverHttp *sock)
	{
		cleanupSocket(sock);
	}

	void sock_closed(WebSocketOverHttp *sock)
	{
		cleanupSocket(sock);
	}

	void sock_error(WebSocketOverHttp *sock)
	{
		cleanupSocket(sock);
	}
};

thread_local WebSocketOverHttp::DisconnectManager *WebSocketOverHttp::g_disconnectManager = 0;
thread_local int WebSocketOverHttp::g_maxManagedDisconnects = -1;

static QList<WebSocketOverHttp::Event> decodeEvents(const QByteArray &in, bool *ok = 0)
{
	QList<WebSocketOverHttp::Event> out;
	if(ok)
		*ok = false;

	int start = 0;
	while(start < in.size())
	{
		int at = in.indexOf("\r\n", start);
		if(at == -1)
			return QList<WebSocketOverHttp::Event>();

		QByteArray typeLine = in.mid(start, at - start);
		start = at + 2;

		WebSocketOverHttp::Event e;
		at = typeLine.indexOf(' ');
		if(at != -1)
		{
			e.type = typeLine.mid(0, at);

			bool check;
			int clen = typeLine.mid(at + 1).toInt(&check, 16);
			if(!check)
				return QList<WebSocketOverHttp::Event>();

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

static QByteArray encodeEvents(const QList<WebSocketOverHttp::Event> &events)
{
	QByteArray out;

	foreach(const WebSocketOverHttp::Event &e, events)
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

class WebSocketOverHttp::Private
{
public:
	struct ReqConnections {
		Connection readyReadConnection;
		Connection bytesWrittenConnection;
		Connection errorConnection;
	};

	WebSocketOverHttp *q;
	ZhttpManager *zhttpManager;
	QString connectHost;
	int connectPort;
	bool ignorePolicies;
	bool trustConnectHost;
	bool ignoreTlsErrors;
	QString clientCert;
	QString clientKey;
	State state;
	QByteArray cid;
	HttpRequestData requestData;
	HttpResponseData responseData;
	ErrorCondition errorCondition;
	ErrorCondition pendingErrorCondition;
	int keepAliveInterval;
	HttpHeaders meta;
	bool updating;
	std::unique_ptr<ZhttpRequest> req;
	QByteArray reqBody;
	int reqPendingBytes;
	int reqFrames;
	int reqContentSize;
	bool reqMaxed;
	bool reqClose;
	int reqCloseContentSize;
	BufferList inBuf;
	QList<Frame> inFrames;
	QList<Frame> outFrames;
	int outContentSize;
	int outFramesReplay;
	int outContentReplay;
	int closeCode;
	QString closeReason;
	bool closeSent;
	bool peerClosing;
	int peerCloseCode;
	QString peerCloseReason;
	bool disconnecting;
	bool disconnectSent;
	bool updateQueued;
	std::unique_ptr<Timer> keepAliveTimer;
	std::unique_ptr<Timer> retryTimer;
	int retries;
	int maxEvents;
	ReqConnections reqConnections;
	Connection keepAliveTimerConnection;
	Connection retryTimerConnection;
	DeferCall deferCall;

	Private(WebSocketOverHttp *_q) :
		q(_q),
		connectPort(-1),
		ignorePolicies(false),
		trustConnectHost(false),
		ignoreTlsErrors(false),
		state(WebSocket::Idle),
		errorCondition(ErrorGeneric),
		pendingErrorCondition((ErrorCondition)-1),
		keepAliveInterval(-1),
		updating(false),
		reqPendingBytes(0),
		reqFrames(0),
		reqContentSize(0),
		reqMaxed(false),
		reqClose(false),
		reqCloseContentSize(0),
		outContentSize(0),
		outFramesReplay(0),
		outContentReplay(0),
		closeCode(-1),
		closeSent(false),
		peerClosing(false),
		peerCloseCode(-1),
		disconnecting(false),
		disconnectSent(false),
		updateQueued(false),
		retries(0),
		maxEvents(0)
	{
		if(!g_disconnectManager)
			g_disconnectManager = new DisconnectManager;

		keepAliveTimer = std::make_unique<Timer>();
		keepAliveTimerConnection = keepAliveTimer->timeout.connect(boost::bind(&Private::keepAliveTimer_timeout, this));
		keepAliveTimer->setSingleShot(true);

		retryTimer = std::make_unique<Timer>();
		retryTimerConnection = retryTimer->timeout.connect(boost::bind(&Private::retryTimer_timeout, this));
		retryTimer->setSingleShot(true);
	}

	void cleanup()
	{
		keepAliveTimer->stop();
		retryTimer->stop();

		updating = false;
		disconnecting = false;
		updateQueued = false;

		reqConnections = ReqConnections();
		req.reset();

		state = Idle;
	}

	void sanitizeRequestHeaders()
	{
		// Don't forward certain headers
		requestData.headers.removeAll("Upgrade");
		requestData.headers.removeAll("Accept");
		requestData.headers.removeAll("Connection-Id");
		requestData.headers.removeAll("Content-Length");

		// Don't forward headers starting with Meta-*
		for(int n = 0; n < requestData.headers.count(); ++n)
		{
			const HttpHeader &h = requestData.headers[n];
			if(qstrnicmp(h.first.data(), "Meta-", 5) == 0)
			{
				requestData.headers.removeAt(n);
				--n; // Adjust position
			}
		}
	}

	void start()
	{
		state = Connecting;

		if(cid.isEmpty())
			cid = UuidUtil::createUuid();

		if(requestData.uri.scheme() == "wss")
			requestData.uri.setScheme("https");
		else
			requestData.uri.setScheme("http");

		update();
	}

	void writeFrame(const Frame &frame)
	{
		assert(state == Connected);

		outFrames += frame;
		outContentSize += frame.data.size();

		if(needUpdate())
			update();
	}

	Frame readFrame()
	{
		return inFrames.takeFirst();
	}

	void close(int code, const QString &reason)
	{
		assert(state != Closing);

		state = Closing;
		closeCode = code;
		closeReason = reason;

		update();
	}

	int writeBytesAvailable() const
	{
		if(outContentSize < BUFFER_SIZE)
			return BUFFER_SIZE - outContentSize;
		else
			return 0;
	}

	void sendDisconnect()
	{
		disconnecting = true;

		update();
	}

	void refresh()
	{
		// Only allow refresh requests if connected
		if(state == Connected && !disconnecting)
		{
			if(!updating)
				update();
			else
				updateQueued = true;
		}
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

	void appendInMessage(Frame::Type type, const QByteArray &message)
	{
		// Split into frames to avoid credits issue
		QList<Frame> frames;

		for(int n = 0; frames.isEmpty() || n < message.size(); n += FRAME_SIZE_MAX)
		{
			Frame::Type ftype;
			if(n == 0)
				ftype = type;
			else
				ftype = Frame::Continuation;

			QByteArray data = message.mid(n, FRAME_SIZE_MAX);
			bool more = (n + FRAME_SIZE_MAX < message.size());

			frames += Frame(ftype, data, more);
		}

		foreach(const Frame &f, frames)
			inFrames += f;
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
		// Always send this right away
		if(disconnecting && !disconnectSent)
			return true;

		if(updateQueued)
			return true;

		bool cscm = canSendCompleteMessage();

		if(!cscm && writeBytesAvailable() == 0)
		{
			// Write buffer maxed with incomplete message. This is
			// unrecoverable. Update to throw error right away.
			return true;
		}

		// If we can't fit a response then don't update yet
		if(!canReceive())
			return false;

		bool newData = outFrames.count() > outFramesReplay || outContentSize > outContentReplay;

		// Have message to send or close?
		if((cscm && newData) || (outFrames.isEmpty() && state == Closing && !closeSent))
			return true;

		return false;
	}

	void queueError(ErrorCondition e)
	{
		if((int)pendingErrorCondition == -1)
		{
			pendingErrorCondition = e;
			deferCall.defer([=] { doError(); });
		}
	}

	void update()
	{
		// Only one request allowed at a time
		if(updating)
			return;

		updateQueued = false;

		updating = true;

		keepAliveTimer->stop();

		// If we can't send yet but also have no room for writes, then fail
		if(!canSendCompleteMessage() && writeBytesAvailable() == 0)
		{
			updating = false;
			queueError(ErrorGeneric);
			return;
		}

		reqFrames = 0;
		reqContentSize = 0;
		reqMaxed = false;
		reqClose = false;
		reqCloseContentSize = 0;

		QList<Event> events;

		if(state == Connecting)
		{
			events += Event("OPEN");
		}
		else if(disconnecting && !disconnectSent)
		{
			events += Event("DISCONNECT");
			disconnectSent = true;
		}
		else
		{
			bool ok = false;
			events += framesToEvents(outFrames, BUFFER_SIZE, maxEvents, &ok, &reqFrames, &reqContentSize);
			if(!ok)
			{
				updating = false;
				queueError(ErrorGeneric);
				return;
			}

			// Set this if we couldn't fit everything
			reqMaxed = reqFrames < outFrames.count();

			if(state == Closing && (maxEvents <= 0 || events.count() < maxEvents))
			{
				if(reqFrames < outFrames.count())
					log_debug("woh: skipping partial message at close");

				if(closeCode != -1)
				{
					QByteArray rawReason = closeReason.toUtf8();

					QByteArray buf(2 + rawReason.size(), 0);
					buf[0] = (closeCode >> 8) & 0xff;
					buf[1] = closeCode & 0xff;
					memcpy(buf.data() + 2, rawReason.data(), rawReason.size());
					events += Event("CLOSE", buf);

					reqCloseContentSize = buf.size();
				}
				else
					events += Event("CLOSE");

				reqClose = true;
			}
		}

		reqBody = encodeEvents(events);

		doRequest();
	}

	void doRequest()
	{
		assert(!req);

		q->aboutToSendRequest();

		req = std::unique_ptr<ZhttpRequest>(zhttpManager->createRequest());
		reqConnections = {
			req->readyRead.connect(boost::bind(&Private::req_readyRead, this)),
			req->bytesWritten.connect(boost::bind(&Private::req_bytesWritten, this, boost::placeholders::_1)),
			req->error.connect(boost::bind(&Private::req_error, this))
		};

		if(!connectHost.isEmpty())
			req->setConnectHost(connectHost);
		if(connectPort != -1)
			req->setConnectPort(connectPort);
		req->setIgnorePolicies(ignorePolicies);
		req->setTrustConnectHost(trustConnectHost);
		req->setIgnoreTlsErrors(ignoreTlsErrors);
		if(!clientCert.isEmpty())
			req->setClientCert(clientCert, clientKey);
		req->setSendBodyAfterAcknowledgement(true);

		HttpHeaders headers = requestData.headers;

		headers += HttpHeader("Accept", "application/websocket-events");
		headers += HttpHeader("Connection-Id", cid);
		headers += HttpHeader("Content-Type", "application/websocket-events");
		headers += HttpHeader("Content-Length", QByteArray::number(reqBody.size()));

		if(outContentReplay > 0)
			headers += HttpHeader("Content-Bytes-Replayed", QByteArray::number(outContentReplay));

		foreach(const HttpHeader &h, meta)
			headers += HttpHeader("Meta-" + h.first, h.second);

		reqPendingBytes = reqBody.size();

		req->start("POST", requestData.uri, headers);
		req->writeBody(reqBody);
		req->endBody();
	}

	void req_readyRead()
	{
		if(inBuf.size() + req->bytesAvailable() > RESPONSE_BODY_MAX)
		{
			cleanup();
			q->error();
			return;
		}

		inBuf += req->readBody();

		if(!req->isFinished())
		{
			// If request isn't finished yet, keep waiting
			return;
		}

		reqBody.clear();
		retries = 0;

		int responseCode = req->responseCode();
		QByteArray responseReason = req->responseReason();
		HttpHeaders responseHeaders = req->responseHeaders();
		QByteArray responseBody = inBuf.take();

		reqConnections = ReqConnections();
		req.reset();

		if(state == Connecting)
		{
			// Save the initial response
			responseData.code = responseCode;
			responseData.reason = responseReason;
			responseData.headers = responseHeaders;
		}

		QByteArray contentType = responseHeaders.get("Content-Type").asQByteArray();

		if(responseCode != 200 || contentType != "application/websocket-events")
		{
			if(state == Connecting)
			{
				errorCondition = ErrorRejected;
				responseData.body = responseBody.mid(0, REJECT_BODY_MAX);
			}
			else
				errorCondition = ErrorGeneric;

			cleanup();
			q->error();
			return;
		}

		if(responseHeaders.contains("Keep-Alive-Interval"))
		{
			bool ok;
			int x = responseHeaders.get("Keep-Alive-Interval").asQByteArray().toInt(&ok);
			if(ok && x > 0)
			{
				if(x < 20)
					x = 20;

				keepAliveInterval = x;
			}
			else
				keepAliveInterval = -1;
		}

		int reqContentSizeWithClose = reqContentSize + reqCloseContentSize;

		// By default, accept all
		int contentBytesAccepted = reqContentSizeWithClose;

		if(responseHeaders.contains("Content-Bytes-Accepted"))
		{
			bool ok;
			int x = responseHeaders.get("Content-Bytes-Accepted").asQByteArray().toInt(&ok);
			if(ok && x >= 0)
				contentBytesAccepted = x;

			// Can't accept more than was offered
			if(contentBytesAccepted > reqContentSizeWithClose)
			{
				cleanup();
				q->error();
				return;
			}
		}

		int nonCloseContentBytesAccepted = qMin(contentBytesAccepted, reqContentSize);

		int outFramesCountOrig = outFrames.count();
		int contentRemoved = removeContentFromFrames(&outFrames, nonCloseContentBytesAccepted);
		int framesRemoved = outFramesCountOrig - outFrames.count();

		// Guaranteed to succeed, since reqContentSize represents the initial
		// data in outFrames and we guard against too large of an input
		assert(contentRemoved == nonCloseContentBytesAccepted);

		outContentSize -= contentRemoved;

		// If we couldn't fit all pending data in the request, then require
		// progress to be made
		if(reqMaxed && framesRemoved == 0 && contentRemoved == 0)
		{
			updating = false;
			queueError(ErrorGeneric);
			return;
		}

		// FramesRemoved could exceed reqFrames if any zero-sized frames are
		// appended to outFrames before acceptance, causing them to be
		// removed even though they weren't in the request
		outFramesReplay = qMax(reqFrames - framesRemoved, 0);

		outContentReplay = reqContentSize - contentRemoved;

		if(reqClose)
		{
			if(nonCloseContentBytesAccepted < reqContentSize)
			{
				// If server didn't accept all content before close, then don't close yet
				reqClose = false;
			}
			else
			{
				// Server accepted all content before close. In that case, we
				// require the server to also accept the close. Partial
				// acceptance is meant for waiting for more data and from
				// this point there won't be any more data.
				if(contentBytesAccepted < reqContentSizeWithClose)
				{
					cleanup();
					q->error();
					return;
				}
			}
		}

		// After close, remove any partial message left
		if(reqClose)
		{
			outFrames.clear();
			outContentSize = 0;
			outFramesReplay = 0;
			outContentReplay = 0;
		}

		foreach(const HttpHeader &h, responseHeaders)
		{
			if(h.first.size() >= 10 && qstrnicmp(h.first.data(), "Set-Meta-", 9) == 0)
			{
				QByteArray name = h.first.mid(9).asQByteArray();
				if(meta.contains(name))
					meta.removeAll(name);
				if(!h.second.isEmpty())
					meta += HttpHeader(name, h.second);
			}
		}

		bool ok;
		QList<Event> events = decodeEvents(responseBody, &ok);
		if(!ok)
		{
			cleanup();
			q->error();
			return;
		}

		if(state == Connecting)
		{
			// Server must respond with events or enable keep alive
			if(events.isEmpty() && keepAliveInterval == -1)
			{
				cleanup();
				q->error();
				return;
			}

			// First event must be OPEN
			if(!events.isEmpty() && events.first().type != "OPEN")
			{
				cleanup();
				q->error();
				return;
			}

			// Correct the status code/reason
			responseData.code = 101;
			responseData.reason = "Switching Protocols";

			// Strip private headers from the initial response
			responseData.headers.removeAll("Content-Length");
			responseData.headers.removeAll("Content-Type");
			responseData.headers.removeAll("Keep-Alive-Interval");
			for(int n = 0; n < responseData.headers.count(); ++n)
			{
				const HttpHeader &h = responseData.headers[n];
				if(h.first.size() >= 10 && qstrnicmp(h.first.data(), "Set-Meta-", 9) == 0)
				{
					responseData.headers.removeAt(n);
					--n; // Adjust position
				}
			}
		}

		if(disconnectSent)
		{
			cleanup();
			q->disconnected();
			return;
		}

		std::weak_ptr<Private> self = q->d;

		bool emitConnected = false;
		bool emitReadyRead = false;
		bool closed = false;
		bool disconnected = false;

		foreach(const Event &e, events)
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
				appendInMessage(Frame::Text, e.content);
				emitReadyRead = true;
			}
			else if(e.type == "BINARY")
			{
				appendInMessage(Frame::Binary, e.content);
				emitReadyRead = true;
			}
			else if(e.type == "PING")
			{
				appendInMessage(Frame::Ping, e.content);
				emitReadyRead = true;
			}
			else if(e.type == "PONG")
			{
				appendInMessage(Frame::Pong, e.content);
				emitReadyRead = true;
			}
			else if(e.type == "CLOSE")
			{
				peerClosing = true;
				if(e.content.size() >= 2)
				{
					int hi = (unsigned char)e.content[0];
					int lo = (unsigned char)e.content[1];
					peerCloseCode = (hi << 8) + lo;
					peerCloseReason = QString::fromUtf8(e.content.mid(2));
				}

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
			q->connected();
			if(self.expired())
				return;
		}

		if(emitReadyRead)
		{
			q->readyRead();
			if(self.expired())
				return;
		}

		if(framesRemoved > 0 || contentRemoved > 0)
		{
			q->framesWritten(framesRemoved, contentRemoved);
			if(self.expired())
				return;
		}

		if(contentRemoved > 0)
		{
			q->writeBytesChanged();
			if(self.expired())
				return;
		}

		if(reqClose)
			closeSent = true;

		if(closed)
		{
			if(closeSent)
			{
				cleanup();
				q->closed();
				return;
			}
			else
			{
				q->peerClosed();
			}
		}
		else if(closeSent && keepAliveInterval == -1)
		{
			// If there are no keep alives, then the server has only one
			// chance to respond to a close. If it doesn't, then
			// consider the connection uncleanly disconnected.
			disconnected = true;
		}

		if(disconnected)
		{
			cleanup();
			q->error();
			return;
		}

		if(reqClose && peerClosing)
		{
			cleanup();
			q->closed();
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
		reqPendingBytes -= count;
		assert(reqPendingBytes >= 0);
	}

	void req_error()
	{
		bool retry = false;

		ZhttpRequest::ErrorCondition reqError = req->errorCondition();

		switch(reqError)
		{
			case ZhttpRequest::ErrorConnect:
			case ZhttpRequest::ErrorConnectTimeout:
			case ZhttpRequest::ErrorTls:
				// These errors mean the server wasn't reached at all
				retry = true;
				break;
			case ZhttpRequest::ErrorGeneric:
			case ZhttpRequest::ErrorTimeout:
				// These errors mean the server may have been reached, so
				// only retry if the request body wasn't completely sent
				if(reqPendingBytes > 0)
					retry = true;
				break;
			default:
				// All other errors are hard fails that shouldn't be retried
				break;
		}

		reqConnections = ReqConnections();
		req.reset();

		if(retry && retries < RETRY_MAX && state != Connecting)
		{
			keepAliveTimer->stop();

			int delay = RETRY_TIMEOUT;
			for(int n = 0; n < retries; ++n)
				delay *= 2;
			delay += (int)(QRandomGenerator::global()->generate() % RETRY_RAND_MAX);

			log_debug("woh: trying again in %dms", delay);

			++retries;

			// This should still be flagged, for protection while retrying
			assert(updating);

			retryTimer->start(delay);
			return;
		}

		if(reqError == ZhttpRequest::ErrorConnect)
			errorCondition = WebSocket::ErrorConnect;
		else if(reqError == ZhttpRequest::ErrorConnectTimeout)
			errorCondition = WebSocket::ErrorConnectTimeout;
		else if(reqError == ZhttpRequest::ErrorTls)
			errorCondition = WebSocket::ErrorTls;

		cleanup();
		q->error();
	}

	void keepAliveTimer_timeout()
	{
		update();
	}

	void retryTimer_timeout()
	{
		doRequest();
	}

	void doError()
	{
		cleanup();
		errorCondition = pendingErrorCondition;
		pendingErrorCondition = (ErrorCondition)-1;
		q->error();
	}
};

void WebSocketOverHttp::DisconnectManager::deleteSocket(WebSocketOverHttp *sock)
{
	// Ensure state is Idle to prevent the destructor from re-adding it to the manager
	sock->d->cleanup();

	delete sock;
}

WebSocketOverHttp::WebSocketOverHttp(ZhttpManager *zhttpManager)
{
	d = std::make_shared<Private>(this);
	d->zhttpManager = zhttpManager;
}

WebSocketOverHttp::WebSocketOverHttp() = default;

WebSocketOverHttp::~WebSocketOverHttp()
{
	if(d->state != Idle && !g_disconnectManager->contains(this) && (g_maxManagedDisconnects < 0 || g_disconnectManager->count() < g_maxManagedDisconnects))
	{
		// If we get destructed while active, clean up in the background
		WebSocketOverHttp *sock = new WebSocketOverHttp;
		sock->d = d;
		d->q = sock;
		d.reset();
		g_disconnectManager->addSocket(sock);
	}
}

void WebSocketOverHttp::setConnectionId(const QByteArray &id)
{
	d->cid = id;
}

void WebSocketOverHttp::setMaxEventsPerRequest(int max)
{
	d->maxEvents = max;
}

void WebSocketOverHttp::refresh()
{
	d->refresh();
}

void WebSocketOverHttp::setMaxManagedDisconnects(int max)
{
	g_maxManagedDisconnects = max;
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
	// This class is client only
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

void WebSocketOverHttp::setTrustConnectHost(bool on)
{
	d->trustConnectHost = on;
}

void WebSocketOverHttp::setIgnoreTlsErrors(bool on)
{
	d->ignoreTlsErrors = on;
}

void WebSocketOverHttp::setClientCert(const QString &cert, const QString &key)
{
	d->clientCert = cert;
	d->clientKey = key;
}

void WebSocketOverHttp::start(const QUrl &uri, const HttpHeaders &headers)
{
	assert(d->state == Idle);

	d->requestData.uri = uri;
	d->requestData.headers = headers;

	d->sanitizeRequestHeaders();

	d->start();
}

void WebSocketOverHttp::respondSuccess(const QByteArray &reason, const HttpHeaders &headers)
{
	Q_UNUSED(reason);
	Q_UNUSED(headers);

	// This class is client only
	assert(0);
}

void WebSocketOverHttp::respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
{
	Q_UNUSED(code);
	Q_UNUSED(reason);
	Q_UNUSED(headers);
	Q_UNUSED(body);

	// This class is client only
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

int WebSocketOverHttp::writeBytesAvailable() const
{
	return d->writeBytesAvailable();
}

int WebSocketOverHttp::peerCloseCode() const
{
	return d->peerCloseCode;
}

QString WebSocketOverHttp::peerCloseReason() const
{
	return d->peerCloseReason;
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

void WebSocketOverHttp::close(int code, const QString &reason)
{
	d->close(code, reason);
}

void WebSocketOverHttp::setHeaders(const HttpHeaders &headers)
{
	d->requestData.headers = headers;

	d->sanitizeRequestHeaders();
}

QList<WebSocketOverHttp::Event> WebSocketOverHttp::framesToEvents(const QList<Frame> &frames, int eventsMax, int contentMax, bool *ok, int *framesRepresented, int *contentRepresented)
{
	QList<WebSocketOverHttp::Event> out;
	int pos = 0;
	int contentSize = 0;

	while(pos < frames.count() && (eventsMax <= 0 || out.count() < eventsMax) && (contentMax <= 0 || contentSize < contentMax))
	{
		// Make sure the next message is fully readable
		int takeCount = -1;
		for(int n = pos; n < frames.count(); ++n)
		{
			if(!frames[n].more)
			{
				takeCount = n - pos + 1;
				break;
			}
		}
		if(takeCount < 1)
			break;

		Frame::Type ftype = Frame::Text;
		BufferList content;

		for(int n = 0; n < takeCount; ++n)
		{
			Frame f = frames[pos + n];

			if((n == 0 && f.type == Frame::Continuation) || (n > 0 && f.type != Frame::Continuation))
			{
				*ok = false;
				return QList<WebSocketOverHttp::Event>();
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

		// For compactness, we only include content on ping/pong if non-empty
		if(ftype == Frame::Text)
			out += Event("TEXT", data);
		else if(ftype == Frame::Binary)
			out += Event("BINARY", data);
		else if(ftype == Frame::Ping)
			out += Event("PING", !data.isEmpty() ? data : QByteArray());
		else if(ftype == Frame::Pong)
			out += Event("PONG", !data.isEmpty() ? data : QByteArray());

		pos += takeCount;
		contentSize += content.size();
	}

	*ok = true;
	*framesRepresented = pos;
	*contentRepresented = contentSize;

	return out;
}

int WebSocketOverHttp::removeContentFromFrames(QList<WebSocket::Frame> *frames, int count)
{
	int left = count;

	while(!frames->isEmpty())
	{
		WebSocket::Frame &f = frames->first();

		// Not allowed
		if(f.type == WebSocket::Frame::Continuation)
			break;

		int size = qMin(left, f.data.size());
		left -= size;

		if(size < f.data.size())
		{
			assert(left == 0);

			if(size > 0)
				f.data = f.data.mid(size);

			break;
		}

		WebSocket::Frame::Type ftype = f.type;
		bool more = f.more;

		// Only remove frame of a multipart message if we can carry over
		// the type
		if(more && frames->count() < 2)
			break;

		frames->removeFirst();

		// If removed frame was part of a multipart message, carry over
		// the type
		if(more)
		{
			assert(!frames->isEmpty());
			frames->first().type = ftype;
		}
	}

	return (count - left);
}
