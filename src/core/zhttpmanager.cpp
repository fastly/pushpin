/*
 * Copyright (C) 2012-2021 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#include "zhttpmanager.h"

#include <assert.h>
#include <QStringList>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "log.h"
#include "zutil.h"
#include "logutil.h"
#include "timer.h"

#define OUT_HWM 100
#define IN_HWM 100
#define DEFAULT_HWM 101000
#define CLIENT_WAIT_TIME 0
#define CLIENT_STREAM_WAIT_TIME 500
#define SERVER_WAIT_TIME 500

#define PENDING_MAX 100

#define REFRESH_INTERVAL 1000
#define ZHTTP_EXPIRE 60000

#define ZHTTP_SHOULD_PROCESS (ZHTTP_EXPIRE * 3 / 4)
#define ZHTTP_REFRESH_BUCKETS (ZHTTP_SHOULD_PROCESS / REFRESH_INTERVAL)

// needs to match the peer
#define ZHTTP_IDS_MAX 128

class ZhttpManager::Private : public QObject
{
	Q_OBJECT

public:
	enum SessionType
	{
		UnknownSession,
		HttpSession,
		WebSocketSession
	};

	class KeepAliveRegistration
	{
	public:
		SessionType type;
		union { ZhttpRequest *req; ZWebSocket *sock; } p;
		int refreshBucket;
	};

	ZhttpManager *q;
	QStringList client_out_specs;
	QStringList client_out_stream_specs;
	QStringList client_in_specs;
	QStringList client_req_specs;
	QStringList server_in_specs;
	QStringList server_in_stream_specs;
	QStringList server_out_specs;
	CacheConfig cache_config;
	std::unique_ptr<QZmq::Socket> client_out_sock;
	std::unique_ptr<QZmq::Socket> client_out_stream_sock;
	std::unique_ptr<QZmq::Socket> client_in_sock;
	std::unique_ptr<QZmq::Socket> client_req_sock;
	std::unique_ptr<QZmq::Socket> server_in_sock;
	std::unique_ptr<QZmq::Socket> server_in_stream_sock;
	std::unique_ptr<QZmq::Socket> server_out_sock;
	std::unique_ptr<QZmq::Valve> client_in_valve;
	std::unique_ptr<QZmq::Valve> client_out_stream_valve;
	std::unique_ptr<QZmq::Valve> server_in_valve;
	std::unique_ptr<QZmq::Valve> server_in_stream_valve;
	QByteArray instanceId;
	int ipcFileMode;
	bool doBind;
	QHash<ZhttpRequest::Rid, ZhttpRequest*> clientReqsByRid;
	QHash<ZhttpRequest::Rid, ZhttpRequest*> serverReqsByRid;
	QList<ZhttpRequest*> serverPendingReqs;
	QHash<ZWebSocket::Rid, ZWebSocket*> clientSocksByRid;
	QHash<ZWebSocket::Rid, ZWebSocket*> serverSocksByRid;
	QList<ZWebSocket*> serverPendingSocks;
	std::unique_ptr<Timer> refreshTimer;
	QHash<void*, KeepAliveRegistration*> keepAliveRegistrations;
	QSet<KeepAliveRegistration*> sessionRefreshBuckets[ZHTTP_REFRESH_BUCKETS];
	int currentSessionRefreshBucket;
	Connection cosConnection;
	Connection cossConnection;
	Connection sosConnection;
	Connection rrConnection;
	Connection clientConnection;
	Connection clientOutStreamConnection;
	Connection serverConnection;
	Connection serverStreamConnection;
	Connection refreshTimerConnection;

	Private(ZhttpManager *_q) :
		QObject(_q),
		q(_q),
		ipcFileMode(-1),
		doBind(false),
		currentSessionRefreshBucket(0)
	{
		refreshTimer = std::make_unique<Timer>();
		refreshTimerConnection = refreshTimer->timeout.connect(boost::bind(&Private::refresh_timeout, this));
	}

	~Private()
	{
		while(!serverPendingReqs.isEmpty())
		{
			ZhttpRequest *req = serverPendingReqs.takeFirst();
			serverReqsByRid.remove(req->rid());
			delete req;
		}

		while(!serverPendingSocks.isEmpty())
		{
			ZWebSocket *sock = serverPendingSocks.takeFirst();
			serverSocksByRid.remove(sock->rid());
			delete sock;
		}

		assert(clientReqsByRid.isEmpty());
		assert(serverReqsByRid.isEmpty());
		assert(clientSocksByRid.isEmpty());
		assert(serverSocksByRid.isEmpty());
		assert(keepAliveRegistrations.isEmpty());
	}

	bool setupClientOut()
	{
		cosConnection.disconnect();
		rrConnection.disconnect();
		client_req_sock.reset();
		client_out_sock.reset();

		client_out_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Push);
		cosConnection = client_out_sock->messagesWritten.connect(boost::bind(&Private::client_out_messagesWritten, this, boost::placeholders::_1));

		client_out_sock->setHwm(OUT_HWM);
		client_out_sock->setShutdownWaitTime(CLIENT_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(client_out_sock.get(), client_out_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	bool setupClientOutStream()
	{
		rrConnection.disconnect();
		cossConnection.disconnect();
		client_req_sock.reset();
		client_out_stream_valve.reset();
		client_out_stream_sock.reset();

		client_out_stream_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);
		cossConnection = client_out_stream_sock->messagesWritten.connect(boost::bind(&Private::client_out_stream_messagesWritten, this, boost::placeholders::_1));

		client_out_stream_sock->setIdentity(instanceId);
		client_out_stream_sock->setWriteQueueEnabled(false);
		client_out_stream_sock->setHwm(DEFAULT_HWM);
		client_out_stream_sock->setShutdownWaitTime(CLIENT_STREAM_WAIT_TIME);
		client_out_stream_sock->setImmediateEnabled(true);

		QString errorMessage;
		if(!ZUtil::setupSocket(client_out_stream_sock.get(), client_out_stream_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		client_out_stream_valve = std::make_unique<QZmq::Valve>(client_out_stream_sock.get());
		clientOutStreamConnection = client_out_stream_valve->readyRead.connect(boost::bind(&Private::client_out_stream_readyRead, this, boost::placeholders::_1));

		client_out_stream_valve->open();

		return true;
	}

	bool setupClientIn()
	{
		rrConnection.disconnect();
		client_req_sock.reset();
		client_in_valve.reset();
		client_in_sock.reset();

		client_in_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Sub);

		client_in_sock->setHwm(DEFAULT_HWM);
		client_in_sock->setShutdownWaitTime(0);
		client_in_sock->subscribe(instanceId + ' ');

		QString errorMessage;
		if(!ZUtil::setupSocket(client_in_sock.get(), client_in_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		client_in_valve = std::make_unique<QZmq::Valve>(client_in_sock.get());
		clientConnection = client_in_valve->readyRead.connect(boost::bind(&Private::client_in_readyRead, this, boost::placeholders::_1));

		client_in_valve->open();

		return true;
	}

	bool setupClientReq()
	{
		cosConnection.disconnect();
		cossConnection.disconnect();
		client_out_sock.reset();
		client_out_stream_sock.reset();
		client_in_sock.reset();

		client_req_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Dealer);
		rrConnection = client_req_sock->readyRead.connect(boost::bind(&Private::client_req_readyRead, this));

		client_req_sock->setHwm(OUT_HWM);
		client_req_sock->setShutdownWaitTime(CLIENT_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(client_req_sock.get(), client_req_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	bool setupServerIn()
	{
		server_in_valve.reset();
		server_in_sock.reset();

		server_in_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Pull);

		server_in_sock->setHwm(IN_HWM);

		QString errorMessage;
		if(!ZUtil::setupSocket(server_in_sock.get(), server_in_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		server_in_valve = std::make_unique<QZmq::Valve>(server_in_sock.get());
		serverConnection = server_in_valve->readyRead.connect(boost::bind(&Private::server_in_readyRead, this, boost::placeholders::_1));

		server_in_valve->open();

		return true;
	}

	bool setupServerInStream()
	{
		serverStreamConnection.disconnect();
		server_in_stream_sock.reset();

		server_in_stream_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);

		server_in_stream_sock->setIdentity(instanceId);
		server_in_stream_sock->setHwm(DEFAULT_HWM);

		QString errorMessage;
		if(!ZUtil::setupSocket(server_in_stream_sock.get(), server_in_stream_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		server_in_stream_valve = std::make_unique<QZmq::Valve>(server_in_stream_sock.get());
		serverStreamConnection = server_in_stream_valve->readyRead.connect(boost::bind(&Private::server_in_stream_readyRead, this, boost::placeholders::_1));

		server_in_stream_valve->open();

		return true;
	}

	bool setupServerOut()
	{
		sosConnection.disconnect();
		server_out_sock.reset();

		server_out_sock = std::make_unique<QZmq::Socket>(QZmq::Socket::Pub);
		sosConnection = server_out_sock->messagesWritten.connect(boost::bind(&Private::server_out_messagesWritten, this, boost::placeholders::_1));

		server_out_sock->setWriteQueueEnabled(false);
		server_out_sock->setHwm(DEFAULT_HWM);
		server_out_sock->setShutdownWaitTime(SERVER_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(server_out_sock.get(), server_out_specs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	int smallestSessionRefreshBucket()
	{
		int best = -1;
		int bestSize = 0;

		for(int n = 0; n < ZHTTP_REFRESH_BUCKETS; ++n)
		{
			if(best == -1 || sessionRefreshBuckets[n].count() < bestSize)
			{
				best = n;
				bestSize = sessionRefreshBuckets[n].count();
			}
		}

		return best;
	}

	void tryRespondCancel(SessionType type, const QByteArray &id, const ZhttpRequestPacket &packet)
	{
		assert(!packet.from.isEmpty());

		// if this was not an error packet, send cancel
		if(packet.type != ZhttpRequestPacket::Error && packet.type != ZhttpRequestPacket::Cancel)
		{
			ZhttpResponsePacket out;
			out.from = instanceId;
			out.ids += ZhttpResponsePacket::Id(id);
			out.type = ZhttpResponsePacket::Cancel;
			write(type, out, packet.from);
		}
	}

	int processRequestForCache(SessionType type, const ZhttpRequestPacket &packet)
	{
		log_debug("[ZHTTPMANAGER] %s", d->cache_config.cacheEnable ? "TRUE" : "FALSE");
		// parse json body
		QVariantMap jsonMap;
		if (parse_jsonMsg(packet.toVariant().toHash().value("body"), jsonMap) < 0)
		{
			log_debug("[WS] failed to parse JSON msg");
			// make invalid
			return -1;
		}
		for(QVariantMap::const_iterator item = jsonMap.begin(); item != jsonMap.end(); ++item) 
		{
			log_debug("key = %s, value = %s", qPrintable(item.key()), qPrintable(item.value().toString().mid(0,128)));
		}

		const ZhttpRequestPacket::Id &id = packet.ids.first();

		return 0;
	}

	int processResponseForCache(SessionType type, const ZhttpResponsePacket &packet)
	{
		// parse json body
		QVariantMap jsonMap;
		if (parse_jsonMsg(packet.toVariant().toHash().value("body"), jsonMap) < 0)
		{
			log_debug("[WS] failed to parse JSON msg");
			// make invalid
			return -1;
		}
		for(QVariantMap::const_iterator item = jsonMap.begin(); item != jsonMap.end(); ++item) 
		{
			log_debug("key = %s, value = %s", qPrintable(item.key()), qPrintable(item.value().toString().mid(0,128)));
		}

		return 0;
	}

	void write(SessionType type, const ZhttpRequestPacket &packet)
	{
		assert(client_out_sock || client_req_sock);
		const char *logprefix = logPrefixForType(type);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(vpacket);

		if(client_out_sock)
		{
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client: OUT1", logprefix);

			int ret = processRequestForCache(type, packet);
			if (ret != 0)
				return;

			client_out_sock->write(QList<QByteArray>() << buf);
		}
		else
		{
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client req: OUT2", logprefix);

			int ret = processRequestForCache(type, packet);
			if (ret != 0)
				return;

			client_req_sock->write(QList<QByteArray>() << QByteArray() << buf);
		}
	}

	void write(SessionType type, const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
	{
		assert(client_out_stream_sock);
		const char *logprefix = logPrefixForType(type);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client: OUT3 %s", logprefix, instanceAddress.data());

		int ret = processRequestForCache(type, packet);
		if (ret != 0)
			return;

		QList<QByteArray> msg;
		msg += instanceAddress;
		msg += QByteArray();
		msg += buf;
		client_out_stream_sock->write(msg);
	}

	void write(SessionType type, const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
	{
		assert(server_out_sock);
		const char *logprefix = logPrefixForType(type);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = instanceAddress + " T" + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s server: OUT %s", logprefix, instanceAddress.data());

		processResponseForCache(type, packet);

		server_out_sock->write(QList<QByteArray>() << buf);
	}

	static const char *logPrefixForType(SessionType type)
	{
		switch(type)
		{
			case HttpSession: return "zhttp";
			case WebSocketSession: return "zws";
			default: return "zhttp/zws";
		}
	}

	void registerKeepAlive(void *p, SessionType type)
	{
		if(keepAliveRegistrations.contains(p))
			return;

		KeepAliveRegistration *r = new KeepAliveRegistration;
		r->type = type;
		if(type == HttpSession)
			r->p.req = (ZhttpRequest *)p;
		else // WebSocketSession
			r->p.sock = (ZWebSocket *)p;

		keepAliveRegistrations.insert(p, r);

		r->refreshBucket = smallestSessionRefreshBucket();
		sessionRefreshBuckets[r->refreshBucket] += r;

		setupKeepAlive();
	}

	void unregisterKeepAlive(void *p)
	{
		KeepAliveRegistration *r = keepAliveRegistrations.value(p);
		if(!r)
			return;

		sessionRefreshBuckets[r->refreshBucket].remove(r);
		keepAliveRegistrations.remove(p);
		delete r;

		setupKeepAlive();
	}

	void setupKeepAlive()
	{
		if(!keepAliveRegistrations.isEmpty())
		{
			if(!refreshTimer->isActive())
				refreshTimer->start(REFRESH_INTERVAL);
		}
		else
			refreshTimer->stop();
	}

	void writeKeepAlive(SessionType type, const QList<ZhttpRequestPacket::Id> &ids, const QByteArray &zhttpAddress)
	{
		ZhttpRequestPacket zreq;
		zreq.from = instanceId;
		zreq.ids = ids;
		zreq.type = ZhttpRequestPacket::KeepAlive;
		write(type, zreq, zhttpAddress);
	}

	void writeKeepAlive(SessionType type, const QList<ZhttpResponsePacket::Id> &ids, const QByteArray &zhttpAddress)
	{
		ZhttpResponsePacket zresp;
		zresp.from = instanceId;
		zresp.ids = ids;
		zresp.type = ZhttpResponsePacket::KeepAlive;
		write(type, zresp, zhttpAddress);
	}

	void client_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void client_out_stream_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void server_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void client_req_readyRead()
	{
		std::weak_ptr<Private> self = q->d;

		while(client_req_sock->canRead())
		{
			QList<QByteArray> msg = client_req_sock->read();
			if(msg.count() != 2)
			{
				log_warning("zhttp/zws client req: received message with parts != 2, skipping");
				continue;
			}

			QByteArray dataRaw = msg[1];
			if(dataRaw.length() < 1 || dataRaw[0] != 'T')
			{
				log_warning("zhttp/zws client req: received message with invalid format (missing type), skipping");
				continue;
			}

			QVariant data = TnetString::toVariant(dataRaw.mid(1));
			if(data.isNull())
			{
				log_warning("zhttp/zws client req: received message with invalid format (tnetstring parse failed), skipping");
				continue;
			}

			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws client req: IN");

			ZhttpResponsePacket p;
			if(!p.fromVariant(data))
			{
				log_warning("zhttp/zws client req: received message with invalid format (parse failed), skipping");
				continue;
			}

			if(p.ids.count() != 1)
			{
				log_warning("zhttp/zws client req: received message with multiple ids, skipping");
				return;
			}

			const ZhttpResponsePacket::Id &id = p.ids.first();

			ZhttpRequest *req = clientReqsByRid.value(ZhttpRequest::Rid(instanceId, id.id));
			if(req)
			{
				req->handle(id.id, id.seq, p);
				if(self.expired())
					return;

				continue;
			}

			log_debug("zhttp/zws client req: received message for unknown request id");

			// NOTE: we don't respond with a cancel message in req mode
		}
	}

	void processClientIn(const QByteArray &receiver, const QByteArray &msg)
	{
		if(msg.length() < 1 || msg[0] != 'T')
		{
			log_warning("zhttp/zws client: received message with invalid format (missing type), skipping");
			return;
		}

		QVariant data = TnetString::toVariant(msg.mid(1));
		if(data.isNull())
		{
			log_warning("zhttp/zws client: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
		{
			if(!receiver.isEmpty())
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws client: IN %s", receiver.data());
			else
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws client: IN");
		}

		ZhttpResponsePacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zhttp/zws client: received message with invalid format (parse failed), skipping");
			return;
		}

		std::weak_ptr<Private> self = q->d;

		foreach(const ZhttpResponsePacket::Id &id, p.ids)
		{
			// is this for a websocket?
			ZWebSocket *sock = clientSocksByRid.value(ZWebSocket::Rid(instanceId, id.id));
			if(sock)
			{
				sock->handle(id.id, id.seq, p);
				if(self.expired())
					return;

				continue;
			}

			// is this for an http request?
			ZhttpRequest *req = clientReqsByRid.value(ZhttpRequest::Rid(instanceId, id.id));
			if(req)
			{
				req->handle(id.id, id.seq, p);
				if(self.expired())
					return;

				continue;
			}

			log_debug("zhttp/zws client: received message for unknown request id, skipping");
		}
	}

	void client_out_stream_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 3)
		{
			log_warning("zhttp/zws client: received router message with parts != 3, skipping");
			return;
		}

		processClientIn(QByteArray(), msg[2]);
	}

	void client_in_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 1)
		{
			log_warning("zhttp/zws client: received pub message with parts != 1, skipping");
			return;
		}

		int at = msg[0].indexOf(' ');
		if(at == -1)
		{
			log_warning("zhttp/zws client: received pub message with invalid format, skipping");
			return;
		}

		QByteArray receiver = msg[0].mid(0, at);
		QByteArray dataRaw = msg[0].mid(at + 1);

		processClientIn(receiver, dataRaw);
	}

	void server_in_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 1)
		{
			log_warning("zhttp/zws server: received message with parts != 1, skipping");
			return;
		}

		if(msg[0].length() < 1 || msg[0][0] != 'T')
		{
			log_warning("zhttp/zws server: received message with invalid format (missing type), skipping");
			return;
		}

		QVariant data = TnetString::toVariant(msg[0].mid(1));
		if(data.isNull())
		{
			log_warning("zhttp/zws server: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws server: IN");

		ZhttpRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zhttp/zws server: received message with invalid format (parse failed), skipping");
			return;
		}

		if(p.from.isEmpty())
		{
			log_warning("zhttp/zws server: received message without from address, skipping");
			return;
		}

		if(p.ids.count() != 1)
		{
			log_warning("zhttp/zws server: received initial message with multiple ids, skipping");
			return;
		}

		const ZhttpRequestPacket::Id &id = p.ids.first();

		if(p.uri.scheme() == "wss" || p.uri.scheme() == "ws")
		{
			ZWebSocket::Rid rid(p.from, id.id);

			ZWebSocket *sock = serverSocksByRid.value(rid);
			if(sock)
			{
				log_warning("zws server: received message for existing request id, canceling");
				tryRespondCancel(WebSocketSession, id.id, p);
				return;
			}

			sock = new ZWebSocket;
			if(!sock->setupServer(q, id.id, id.seq, p))
			{
				delete sock;
				return;
			}

			serverSocksByRid.insert(rid, sock);
			serverPendingSocks += sock;

			if(serverPendingReqs.count() + serverPendingSocks.count() >= PENDING_MAX)
				server_in_valve->close();

			q->socketReady();
		}
		else if(p.uri.scheme() == "https" || p.uri.scheme() == "http")
		{
			ZhttpRequest::Rid rid(p.from, id.id);

			ZhttpRequest *req = serverReqsByRid.value(rid);
			if(req)
			{
				log_warning("zhttp server: received message for existing request id, canceling");
				tryRespondCancel(HttpSession, id.id, p);
				return;
			}

			req = new ZhttpRequest;
			if(!req->setupServer(q, id.id, id.seq, p))
			{
				delete req;
				return;
			}

			serverReqsByRid.insert(rid, req);
			serverPendingReqs += req;

			if(serverPendingReqs.count() + serverPendingSocks.count() >= PENDING_MAX)
				server_in_valve->close();

			q->requestReady();
		}
		else
		{
			log_debug("zhttp/zws server: rejecting unsupported scheme: %s", qPrintable(p.uri.scheme()));
			tryRespondCancel(UnknownSession, id.id, p);
			return;
		}
	}

	void server_in_stream_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 3)
		{
			log_warning("zhttp/zws server: received message with parts != 3, skipping");
			return;
		}

		if(msg[2].length() < 1 || msg[2][0] != 'T')
		{
			log_warning("zhttp/zws server: received message with invalid format (missing type), skipping");
			return;
		}

		QVariant data = TnetString::toVariant(msg[2].mid(1));
		if(data.isNull())
		{
			log_warning("zhttp/zws server: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "zhttp/zws server: IN stream");

		ZhttpRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zhttp/zws server: received message with invalid format (parse failed), skipping");
			return;
		}

		std::weak_ptr<Private> self = q->d;

		foreach(const ZhttpRequestPacket::Id &id, p.ids)
		{
			// is this for a websocket?
			ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(p.from, id.id));
			if(sock)
			{
				sock->handle(id.id, id.seq, p);
				if(self.expired())
					return;

				continue;
			}

			// is this for an http request?
			ZhttpRequest *req = serverReqsByRid.value(ZhttpRequest::Rid(p.from, id.id));
			if(req)
			{
				req->handle(id.id, id.seq, p);
				if(self.expired())
					return;

				continue;
			}

			log_debug("zhttp/zws server: received message for unknown request id, skipping");
		}
	}

	void refresh_timeout()
	{
		QHash<QByteArray, QList<KeepAliveRegistration*> > clientSessionsBySender[2]; // index corresponds to type
		QHash<QByteArray, QList<KeepAliveRegistration*> > serverSessionsBySender[2]; // index corresponds to type

		// process the current bucket
		const QSet<KeepAliveRegistration*> &bucket = sessionRefreshBuckets[currentSessionRefreshBucket];
		foreach(KeepAliveRegistration *r, bucket)
		{
			QPair<QByteArray, QByteArray> rid;
			bool isServer;
			if(r->type == HttpSession)
			{
				rid = r->p.req->rid();
				isServer = r->p.req->isServer();
			}
			else // WebSocketSession
			{
				rid = r->p.sock->rid();
				isServer = r->p.sock->isServer();
			}

			QByteArray sender;
			if(isServer)
			{
				sender = rid.first;
			}
			else
			{
				if(r->type == HttpSession)
					sender = r->p.req->toAddress();
				else // WebSocketSession
					sender = r->p.sock->toAddress();
			}

			assert(!sender.isEmpty());

			QHash<QByteArray, QList<KeepAliveRegistration*> > &sessionsBySender = (isServer ? serverSessionsBySender[r->type - 1] : clientSessionsBySender[r->type - 1]);

			if(!sessionsBySender.contains(sender))
				sessionsBySender.insert(sender, QList<KeepAliveRegistration*>());

			QList<KeepAliveRegistration*> &sessions = sessionsBySender[sender];
			sessions += r;

			// if we're at max, send out now
			if(sessions.count() >= ZHTTP_IDS_MAX)
			{
				if(isServer)
				{
					QList<ZhttpResponsePacket::Id> ids;
					foreach(KeepAliveRegistration *i, sessions)
					{
						assert(i->type == r->type);
						if(r->type == HttpSession)
							ids += ZhttpResponsePacket::Id(i->p.req->rid().second, i->p.req->outSeqInc());
						else // WebSocketSession
							ids += ZhttpResponsePacket::Id(i->p.sock->rid().second, i->p.sock->outSeqInc());
					}

					writeKeepAlive(r->type, ids, sender);
				}
				else
				{
					QList<ZhttpRequestPacket::Id> ids;
					foreach(KeepAliveRegistration *i, sessions)
					{
						assert(i->type == r->type);
						if(r->type == HttpSession)
							ids += ZhttpRequestPacket::Id(i->p.req->rid().second, i->p.req->outSeqInc());
						else // WebSocketSession
							ids += ZhttpRequestPacket::Id(i->p.sock->rid().second, i->p.sock->outSeqInc());
					}

					writeKeepAlive(r->type, ids, sender);
				}

				sessions.clear();
				sessionsBySender.remove(sender);
			}
		}

		// send last packets
		for(int n = 0; n < 2; ++n)
		{
			SessionType type = (SessionType)(n + 1);

			{
				QHashIterator<QByteArray, QList<KeepAliveRegistration*> > sit(clientSessionsBySender[n]);
				while(sit.hasNext())
				{
					sit.next();
					const QByteArray &sender = sit.key();
					const QList<KeepAliveRegistration*> &sessions = sit.value();

					if(!sessions.isEmpty())
					{
						QList<ZhttpRequestPacket::Id> ids;
						foreach(KeepAliveRegistration *i, sessions)
						{
							assert(i->type == type);
							if(type == HttpSession)
								ids += ZhttpRequestPacket::Id(i->p.req->rid().second, i->p.req->outSeqInc());
							else // WebSocketSession
								ids += ZhttpRequestPacket::Id(i->p.sock->rid().second, i->p.sock->outSeqInc());
						}

						writeKeepAlive(type, ids, sender);
					}
				}
			}

			{
				QHashIterator<QByteArray, QList<KeepAliveRegistration*> > sit(serverSessionsBySender[n]);
				while(sit.hasNext())
				{
					sit.next();
					const QByteArray &sender = sit.key();
					const QList<KeepAliveRegistration*> &sessions = sit.value();

					if(!sessions.isEmpty())
					{
						QList<ZhttpResponsePacket::Id> ids;
						foreach(KeepAliveRegistration *i, sessions)
						{
							assert(i->type == type);
							if(type == HttpSession)
								ids += ZhttpResponsePacket::Id(i->p.req->rid().second, i->p.req->outSeqInc());
							else // WebSocketSession
								ids += ZhttpResponsePacket::Id(i->p.sock->rid().second, i->p.sock->outSeqInc());
						}

						writeKeepAlive(type, ids, sender);
					}
				}
			}
		}

		++currentSessionRefreshBucket;
		if(currentSessionRefreshBucket >= ZHTTP_REFRESH_BUCKETS)
			currentSessionRefreshBucket = 0;
	}

	void parse_jsonMap(QVariantMap& jsonData, QString keyName, QVariantMap& jsonMap)
	{
		for(QVariantMap::const_iterator item = jsonData.begin(); item != jsonData.end(); ++item) 
		{
			QString itemKey = item.key();
			QVariant itemVal = item.value();
			// if has the same key, skip
			if (jsonMap.contains(itemKey))
				continue;

			itemKey = keyName.isNull() ? itemKey : keyName+">>"+itemKey;

			// add exception for id field
			if (itemKey == "id")
			{
				if (itemVal.type() == QVariant::String)
				{
					QString strVal = "\"";
					strVal += itemVal.toString();
					strVal += "\"";
					jsonMap[itemKey] = strVal;
				}
				else if (itemVal.canConvert<QString>())
				{
					jsonMap[itemKey] = itemVal.toString();
				}
			}
			else if (itemVal.canConvert<QString>())
			{
				jsonMap[itemKey] = itemVal.toString();
			}
			else if (itemVal.type() == QVariant::Map)
			{
				QVariantMap mapData = itemVal.toMap();
				parse_jsonMap(mapData, itemKey, jsonMap);
			}
			else if (itemVal.type() == QVariant::List)
			{
				QString tmpStr = "";
				int i = 0;
				for (QVariant m : itemVal.toList())
				{
					if (m.canConvert<QString>())
					{
						tmpStr += m.toString() + "+";
					}
					else if (m.type() == QVariant::List)
					{
						for (QVariant n : m.toList())
						{
							if (n.canConvert<QString>())
							{
								QString s = n.toString();
								if (s.length() == 0)
								{
									tmpStr += "null";
									tmpStr += "+";
								}
								else
								{
									tmpStr += n.toString() + "+";
								}
							}
							else
							{
								log_debug("[WS] invalid type=%s", n.typeName());
							}
						}
						// remove '+', '/' at the end
						while (tmpStr.endsWith("+") || tmpStr.endsWith("/"))
						{
							tmpStr.remove(tmpStr.length()-1, 1);
						}
						tmpStr += "/";
					}
					else if (m.type() == QVariant::Map)
					{
						QVariantMap mapData = m.toMap();
						parse_jsonMap(mapData, itemKey+">>"+QString::number(i), jsonMap);
					}
					i++;
				}

				// remove '+', '/' at the end
				while (tmpStr.endsWith("+") || tmpStr.endsWith("/"))
				{
					tmpStr.remove(tmpStr.length()-1, 1);
				}
				
				jsonMap[itemKey] = (tmpStr.length() > 0) ? tmpStr : "[LIST]";
			}
			else
			{
				log_debug("[WS] unknown parse json type=%s", itemVal.typeName());
			}
		}
	}

	int parse_jsonMsg(QVariant jsonMsg, QVariantMap& jsonMap)
	{
		// parse body as JSON string
		QJsonParseError error;
		QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonMsg.toByteArray(), &error);

		if(error.error != QJsonParseError::NoError)
			return -1;
		
		if(jsonDoc.isObject())
		{
			QVariantMap jsonData = jsonDoc.object().toVariantMap();
			parse_jsonMap(jsonData, NULL, jsonMap);
		}
		else if(jsonDoc.isArray())
		{
			QVariantList jsonData = jsonDoc.array().toVariantList();
			for(const QVariant& item : jsonData) 
			{
				if (item.type() == QVariant::Map)
				{
					QVariantMap mapData = item.toMap();
					parse_jsonMap(mapData, NULL, jsonMap);
					break;
				}
			}
		}
		else
		{
			return -1;
		}

		return 0;
	}
};

ZhttpManager::ZhttpManager(QObject *parent) :
	QObject(parent)
{
	d = std::make_shared<Private>(this);
}

ZhttpManager::~ZhttpManager() = default;

int ZhttpManager::connectionCount() const
{
	int total = 0;
	total += d->clientReqsByRid.count();
	total += d->serverReqsByRid.count();
	total += d->clientSocksByRid.count();
	total += d->serverSocksByRid.count();
	return total;
}

bool ZhttpManager::clientUsesReq() const
{
	return (!d->client_out_sock && d->client_req_sock);
}

ZhttpRequest *ZhttpManager::serverRequestByRid(const ZhttpRequest::Rid &rid) const
{
	return d->serverReqsByRid.value(rid);
}

QByteArray ZhttpManager::instanceId() const
{
	return d->instanceId;
}

void ZhttpManager::setInstanceId(const QByteArray &id)
{
	d->instanceId = id;
}

void ZhttpManager::setIpcFileMode(int mode)
{
	d->ipcFileMode = mode;
}

void ZhttpManager::setBind(bool enable)
{
	d->doBind = enable;
}

bool ZhttpManager::setClientOutSpecs(const QStringList &specs)
{
	d->client_out_specs = specs;
	return d->setupClientOut();
}

bool ZhttpManager::setClientOutStreamSpecs(const QStringList &specs)
{
	d->client_out_stream_specs = specs;
	return d->setupClientOutStream();
}

bool ZhttpManager::setClientInSpecs(const QStringList &specs)
{
	d->client_in_specs = specs;
	return d->setupClientIn();
}

bool ZhttpManager::setClientReqSpecs(const QStringList &specs)
{
	d->client_req_specs = specs;
	return d->setupClientReq();
}

bool ZhttpManager::setServerInSpecs(const QStringList &specs)
{
	d->server_in_specs = specs;
	return d->setupServerIn();
}

bool ZhttpManager::setServerInStreamSpecs(const QStringList &specs)
{
	d->server_in_stream_specs = specs;
	return d->setupServerInStream();
}

bool ZhttpManager::setServerOutSpecs(const QStringList &specs)
{
	d->server_out_specs = specs;
	return d->setupServerOut();
}

void ZhttpManager::setCacheEnable(bool enable)
{
	d->cache_config.cacheEnable = enable;
}

ZhttpRequest *ZhttpManager::createRequest()
{
	ZhttpRequest *req = new ZhttpRequest;
	req->setupClient(this, d->client_req_sock ? true : false);
	return req;
}

ZhttpRequest *ZhttpManager::takeNextRequest()
{
	ZhttpRequest *req = 0;

	while(!req)
	{
		if(d->serverPendingReqs.isEmpty())
			return 0;

		req = d->serverPendingReqs.takeFirst();
		if(!d->serverReqsByRid.contains(req->rid()))
		{
			// this means the object was a zombie. clean up and take next
			delete req;
			req = 0;
			continue;
		}

		d->server_in_valve->open();
	}

	req->startServer();
	return req;
}

ZWebSocket *ZhttpManager::createSocket()
{
	// websockets not allowed in req mode
	assert(!d->client_req_sock);

	ZWebSocket *sock = new ZWebSocket;
	sock->setupClient(this);
	return sock;
}

ZWebSocket *ZhttpManager::takeNextSocket()
{
	ZWebSocket *sock = 0;

	while(!sock)
	{
		if(d->serverPendingSocks.isEmpty())
			return 0;

		sock = d->serverPendingSocks.takeFirst();
		if(!d->serverSocksByRid.contains(sock->rid()))
		{
			// this means the object was a zombie. clean up and take next
			delete sock;
			sock = 0;
			continue;
		}

		d->server_in_valve->open();
	}

	sock->startServer();
	return sock;
}

ZhttpRequest *ZhttpManager::createRequestFromState(const ZhttpRequest::ServerState &state)
{
	ZhttpRequest *req = new ZhttpRequest;
	req->setupServer(this, state);
	return req;
}

void ZhttpManager::link(ZhttpRequest *req)
{
	if(req->isServer())
		d->serverReqsByRid.insert(req->rid(), req);
	else
		d->clientReqsByRid.insert(req->rid(), req);
}

void ZhttpManager::unlink(ZhttpRequest *req)
{
	if(req->isServer())
		d->serverReqsByRid.remove(req->rid());
	else
		d->clientReqsByRid.remove(req->rid());
}

void ZhttpManager::link(ZWebSocket *sock)
{
	if(sock->isServer())
		d->serverSocksByRid.insert(sock->rid(), sock);
	else
		d->clientSocksByRid.insert(sock->rid(), sock);
}

void ZhttpManager::unlink(ZWebSocket *sock)
{
	if(sock->isServer())
		d->serverSocksByRid.remove(sock->rid());
	else
		d->clientSocksByRid.remove(sock->rid());
}

bool ZhttpManager::canWriteImmediately() const
{
	assert(d->client_out_sock || d->client_req_sock);

	if(d->client_out_sock)
		return d->client_out_sock->canWriteImmediately();
	else
		return d->client_req_sock->canWriteImmediately();
}

void ZhttpManager::writeHttp(const ZhttpRequestPacket &packet)
{
	d->write(Private::HttpSession, packet);
}

void ZhttpManager::writeHttp(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::HttpSession, packet, instanceAddress);
}

void ZhttpManager::writeHttp(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::HttpSession, packet, instanceAddress);
}

void ZhttpManager::writeWs(const ZhttpRequestPacket &packet)
{
	d->write(Private::WebSocketSession, packet);
}

void ZhttpManager::writeWs(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::WebSocketSession, packet, instanceAddress);
}

void ZhttpManager::writeWs(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::WebSocketSession, packet, instanceAddress);
}

void ZhttpManager::registerKeepAlive(ZhttpRequest *req)
{
	d->registerKeepAlive(req, Private::HttpSession);
}

void ZhttpManager::unregisterKeepAlive(ZhttpRequest *req)
{
	d->unregisterKeepAlive(req);
}

void ZhttpManager::registerKeepAlive(ZWebSocket *sock)
{
	d->registerKeepAlive(sock, Private::WebSocketSession);
}

void ZhttpManager::unregisterKeepAlive(ZWebSocket *sock)
{
	d->unregisterKeepAlive(sock);
}

int ZhttpManager::estimateRequestHeaderBytes(const QString &method, const QUrl &uri, const HttpHeaders &headers)
{
	int total = method.toUtf8().length();

	total += uri.path(QUrl::FullyEncoded).length();

	if(uri.hasQuery())
		total += uri.query(QUrl::FullyEncoded).length() + 1; // +1 for question mark

	foreach(const HttpHeader &h, headers)
	{
		total += h.first.length();
		total += h.second.length();
	}

	return total;
}

int ZhttpManager::estimateResponseHeaderBytes(int code, const QByteArray &reason, const HttpHeaders &headers)
{
	int total = QString::number(code).length();
	total += reason.length();

	foreach(const HttpHeader &h, headers)
	{
		total += h.first.length();
		total += h.second.length();
	}

	return total;
}

#include "zhttpmanager.moc"
