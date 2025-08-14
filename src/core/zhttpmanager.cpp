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
#include <QCryptographicHash>
#include <QDateTime>
#include <QTimer>
#include <functional>
#include <QtConcurrent>
#include <QThread>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "log.h"
#include "zutil.h"
#include "logutil.h"
#include "timer.h"
#include "cacheutil.h"

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

// max length of one packet in log
#define DEBUG_LOG_MAX_LENGTH	1024

#define CACHE_INTERVAL 1000

#define PING_INTERVAL	20

/////////////////////////////////////////////////////////////////////////////////////
// cache data structure

bool gCacheEnable = false;
QStringList gHttpBackendUrlList;
QStringList gWsBackendUrlList;

QList<ClientItem> gWsCacheClientList;
QHash<QByteArray, int> gWsKilledCacheClientMap;
QHash<QByteArray, time_t> gRestartCacheClientMap;

ZhttpResponsePacket gWsInitResponsePacket;
QHash<QByteArray, ClientItem> gWsClientMap;
QHash<QByteArray, ClientItem> gHttpClientMap;

QList<CacheKeyItem> gCacheKeyItemList;
QString gMsgIdAttrName = "id";
QString gMsgMethodAttrName = "method";
QString gMsgParamsAttrName = "params";
QString gResultAttrName = "result";
QStringList gErrorAttrList;
QString gSubscriptionAttrName = "params>>subscription";
QString gSubscribeBlockAttrName = "params>>result>>block";
QString gSubscribeChangesAttrName = "params>>result>>changes";

int gAccessTimeoutSeconds = 30;
int gResponseTimeoutSeconds = 90;
int gClientNoRequestTimeoutSeconds = 120; // 2mins
int gCacheTimeoutSeconds = 10;
int gShorterTimeoutSeconds = 5;
int gLongerTimeoutSeconds = 60;
int gCacheItemMaxCount = 3000;

QFuture<void> gCacheThread;

QStringList gCacheMethodList;
QHash<QString, QString> gSubscribeMethodMap;
QHash<QByteArray, QList<UnsubscribeRequestItem>> gUnsubscribeRequestMap;
QList<QByteArray> gDeleteClientList;
QStringList gNeverTimeoutMethodList;
QStringList gRefreshShorterMethodList;
QStringList gRefreshLongerMethodList;
QStringList gRefreshUneraseMethodList;
QStringList gRefreshExcludeMethodList;
QStringList gRefreshPassthroughMethodList;
QStringList gNullResponseMethodList;

// multi packets params
QHash<QByteArray, ZhttpResponsePacket> gHttpMultiPartResponseItemMap;
QHash<QByteArray, ZhttpRequestPacket> gWsMultiPartRequestItemMap;
QHash<QByteArray, ZhttpResponsePacket> gWsMultiPartResponseItemMap;

constexpr int CHUNK_SIZE = 8 * 1024; // 8 KB

// prometheus restore allow seconds (default 300)
int gPrometheusRestoreAllowSeconds = 300;

// redis
bool gRedisEnable = false;
QString gRedisHostAddr = "127.0.0.1";
int gRedisPort = 6379;
int gRedisPoolCount = 10;
QString gRedisKeyHeader = "";
bool gReplicaFlag = false;
QString gReplicaMasterAddr = "";
int gReplicaMasterPort = 6379;
bool gReplicaTimerStarted = false;

// count method group
QHash<QString, QStringList> gCountMethodGroupMap;

// prometheus status
QList<QString> gCacheMethodRequestCountList;
QList<QString> gCacheMethodResponseCountList;
quint32 numRequestReceived, numMessageSent, numWsConnect;
quint32 numClientCount, numHttpClientCount, numWsClientCount;
quint32 numRpcAuthor, numRpcBabe, numRpcBeefy, numRpcChain, numRpcChildState;
quint32 numRpcContracts, numRpcDev, numRpcEngine, numRpcEth, numRpcNet;
quint32 numRpcWeb3, numRpcGrandpa, numRpcMmr, numRpcOffchain, numRpcPayment;
quint32 numRpcRpc, numRpcState, numRpcSyncstate, numRpcSystem, numRpcSubscribe;
quint32 numCacheInsert, numCacheHit, numNeverTimeoutCacheInsert, numNeverTimeoutCacheHit;
quint32 numCacheLookup, numCacheExpiry, numRequestMultiPart;
quint32 numSubscriptionInsert, numSubscriptionHit, numSubscriptionLookup, numSubscriptionExpiry, numResponseMultiPart;
quint32 numCacheItem, numAutoRefreshItem, numAREItemCount, numSubscriptionItem, numNeverTimeoutCacheItem;
QHash<QString, int> groupMethodCountMap;
QHash<QString, int> httpCacheClientConnectFailedCountMap;
QHash<QString, int> httpCacheClientInvalidResponseCountMap;
QHash<QString, int> wsCacheClientConnectFailedCountMap;
QHash<QString, int> wsCacheClientInvalidResponseCountMap;

static int gWorkersCount;

/////////////////////////////////////////////////////////////////////////////////////

class ZhttpManager::Private : public QObject
{
	Q_OBJECT

public:
	enum SessionType
	{
		UnknownSession,
		HttpSession,
		WebSocketSession,
		CacheRequest,
		CacheResponse
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

	void send_response_to_client(
		ZhttpResponsePacket::Type packetType,
		const QByteArray &clientId, 
		const QByteArray &from,
		int credits = 0,
		ZhttpResponsePacket *responsePacket = NULL,
		const QByteArray &responseKey = NULL)
	{
		assert(!from.isEmpty());

		ZhttpResponsePacket out;

		ZhttpResponsePacket::Id tempId;

		QByteArray newFrom = from;

		switch (packetType)
		{
		case ZhttpResponsePacket::Data:
			if (responsePacket != NULL)
			{
				out = *responsePacket;
				out.ids[0].id = clientId;
				if (responseKey != NULL)
				{
					out.headers.removeAll("sec-websocket-accept");
					out.headers += HttpHeader("sec-websocket-accept", responseKey);
				}

				// update the counter for prometheus
				gCacheMethodResponseCountList.append(responseKey != NULL ? "WS_INIT" : "WS");
				break;
			}
			return;
		case ZhttpResponsePacket::Credit:
			tempId.id = clientId;
			out.ids += tempId;
			out.type = packetType;
			out.credits = credits;
			break;
		default:
			tempId.id = clientId;
			out.ids += tempId;
			out.type = packetType;
			break;
		}

		out.from = instanceId;//clientInstanceId;
		writeToClient(CacheResponse, out, newFrom);
	}

	void write(SessionType type, const ZhttpRequestPacket &packet)
	{
		assert(client_out_sock || client_req_sock);
		const char *logprefix = logPrefixForType(type);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(vpacket);

		if(client_out_sock)
		{
			if(log_outputLevel() > LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client: OUT1", logprefix);

			client_out_sock->write(QList<QByteArray>() << buf);
		}
		else
		{
			if(log_outputLevel() > LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client req: OUT2", logprefix);

			client_req_sock->write(QList<QByteArray>() << QByteArray() << buf);
		}
	}

	void write(SessionType type, const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
	{
		assert(client_out_stream_sock);
		const char *logprefix = logPrefixForType(type);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(vpacket);

		if(log_outputLevel() > LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s client: OUT3 %s", logprefix, instanceAddress.data());

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

		QByteArray packetId = packet.ids.first().id;

		// cache process
		if (gCacheEnable == true && type != SessionType::CacheRequest && type != SessionType::CacheResponse)
		{
			pause_cache_thread();

			int ccIndex = get_cc_index_from_clientId(packetId);
			// update data receive time
			if (ccIndex >= 0)
			{
				gWsCacheClientList[ccIndex].lastResponseTime = QDateTime::currentMSecsSinceEpoch();
			}

			if (packet.code == 101) // ws client init response code
			{
				if (ccIndex >= 0)
				{
					// cache client
					gWsCacheClientList[ccIndex].initFlag = true;
					gWsCacheClientList[ccIndex].lastResponseTime = QDateTime::currentMSecsSinceEpoch();
					gWsCacheClientList[ccIndex].lastResponseSeq = -1;
					gWsCacheClientList[ccIndex].receiver = packet.from;
					log_debug("[WS] Initialized Cache client%d, %s, from=%s", ccIndex, gWsCacheClientList[ccIndex].clientId.data(),
						packet.from.toHex().data());
					gWsInitResponsePacket = packet;
				}
				else
				{
					// real client
					log_debug("[WS] Initialized real client=%s", packetId.data());
				}
			}
			else
			{
				switch (packet.type)
				{
				case ZhttpResponsePacket::Cancel:
				case ZhttpResponsePacket::Close:
				case ZhttpResponsePacket::Error:
					{
						log_debug("[WS] switching client of response error, condition=%s", packet.condition.data());

						// get error type
						QString conditionStr = QString(packet.condition);
						if (conditionStr.compare("remote-connection-failed", Qt::CaseInsensitive) == 0 ||
							conditionStr.compare("connection-timeout", Qt::CaseInsensitive) == 0)
						{
							log_debug("[WS] Sleeping for 10 seconds");
							sleep(10);
						}

						// if cache client0 is ON, start cache client1
						int ccIndex = get_cc_index_from_clientId(packetId);
						if (ccIndex >= 0)
						{
							log_debug("[WS] disabled cache client %d", ccIndex);
							QString urlPath = gWsBackendUrlList[ccIndex];
							wsCacheClientConnectFailedCountMap[urlPath]++;
							gWsCacheClientList[ccIndex].initFlag = false;
						}
					}
					break;
				case ZhttpResponsePacket::Credit:
					log_debug("[WS] passing credit response");
					break;
				case ZhttpResponsePacket::Ping:
					log_debug("[WS] passing ping response");
					break;
				case ZhttpResponsePacket::KeepAlive:
					log_debug("[WS] passing keep-alive response");
					break;
				case ZhttpResponsePacket::Data:
					if (ccIndex >= 0)
					{
						// increase credit
						int creditSize = static_cast<int>(packet.body.size());
						ZhttpRequestPacket out;
						out.type = ZhttpRequestPacket::Credit;
						out.credits = creditSize;
						send_ws_request_over_cacheclient(out, NULL, ccIndex);

						process_ws_cacheclient_response(packet, ccIndex, instanceAddress);
						resume_cache_thread();
						return;
					}
					else
					{
						int ret = process_http_response(packet, instanceAddress);
						if (ret == 0)
						{
							resume_cache_thread();
							return;
						}
					}
					break;
				default:
					break;
				}
			}

			resume_cache_thread();
		}

		ZhttpResponsePacket p = packet;

		p.ids.first().seq = get_client_new_response_seq(packetId);
		QVariant vpacket = p.toVariant();
		QByteArray buf = instanceAddress + " T" + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s server: OUT %s", logprefix, instanceAddress.data());

		server_out_sock->write(QList<QByteArray>() << buf);
	}

	void writeToClient(SessionType type, ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
	{
		assert(server_out_sock);
		const char *logprefix = logPrefixForType(type);

		QByteArray clientId = packet.ids.first().id;
		int newSeq = get_client_new_response_seq(clientId);
		if (newSeq < 0)
		{
			log_debug("[WS] failed to get new response seq %s", clientId.constData());
			return;
		}
		packet.ids.first().seq = newSeq;

		QVariant vpacket = packet.toVariant();
		QByteArray buf = instanceAddress + " T" + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s server: OUT %s", logprefix, instanceAddress.data()); 

		server_out_sock->write(QList<QByteArray>() << buf);
	}

	void writeToClient_(const QByteArray &cacheItemId, const QByteArray &clientId, const QString &msgId, const QByteArray &instanceAddress, const QByteArray &instanceId)
	{
		assert(server_out_sock);

		int newSeq = get_client_new_response_seq(clientId);
		if (newSeq < 0)
		{
			log_debug("[WS] failed_ to get new response seq %s", clientId.constData());
			return;
		}

		QByteArray buf = load_cache_response_buffer(instanceAddress, cacheItemId, clientId, newSeq, msgId, instanceId, 0);

		// count methods
		numMessageSent++;

		server_out_sock->write(QList<QByteArray>() << buf);
		QThread::usleep(1);
	}

	void writeToClient__(SessionType type, ZhttpResponsePacket &packet, const QByteArray &clientId, const QByteArray &instanceAddress, const QByteArray &instanceId)
	{
		assert(server_out_sock);
		const char *logprefix = logPrefixForType(type);

		/*
		packet.ids.first().id = clientId;
		int newSeq = get_client_new_response_seq(clientId);
		if (newSeq < 0)
		{
			log_debug("[WS] failed to get new response seq %s", clientId.constData());
			return;
		}
		packet.ids.first().seq = newSeq;
		packet.from = instanceId;

		QVariant vpacket = packet.toVariant();
		QByteArray buf = instanceAddress + " T" + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s server: OUT %s", logprefix, instanceAddress.data()); 
		*/
		// count methods
		numMessageSent++;

		//server_out_sock->write(QList<QByteArray>() << buf);
		QByteArray packetBody = packet.body;
		QList<QByteArray> chunks;
		int offset = 0;
		while (offset < packetBody.size()) 
		{
			int len = qMin(CHUNK_SIZE, packetBody.size() - offset);
			chunks << packetBody.mid(offset, len);
			offset += len;
		}

		foreach (QByteArray chunk, chunks)
		{
			ZhttpResponsePacket p;
			ZhttpRequestPacket::Id tempId;
			tempId.id = clientId;
			tempId.seq = get_client_new_response_seq(clientId);
			p.ids += tempId;
			p.from = instanceId;
			p.body = chunk;

			QVariant vpacket = p.toVariant();
			QByteArray buf = instanceAddress + " T" + TnetString::fromVariant(vpacket);
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s server: OUT %s", logprefix, instanceAddress.data()); 
			
			server_out_sock->write(QList<QByteArray>() << buf);
		}

		server_out_sock->write(chunks);
	}

	void writeToClient1__(SessionType type, ZhttpResponsePacket &packet, const QByteArray &clientId, const QByteArray &instanceAddress, const QByteArray &instanceId)
	{
		assert(server_out_sock);
		const char *logprefix = logPrefixForType(type);

		packet.ids.first().id = clientId;
		int newSeq = get_client_new_response_seq(clientId);
		if (newSeq < 0)
		{
			log_debug("[WS] failed to get new response seq %s", clientId.constData());
			return;
		}
		packet.ids.first().seq = newSeq;
		packet.from = instanceId;

		QVariant vpacket = packet.toVariant();
		QByteArray buf = instanceAddress + " T" + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s server: OUT %s", logprefix, instanceAddress.data()); 

		// count methods
		numMessageSent++;

		server_out_sock->write(QList<QByteArray>() << buf);
	}

	static const char *logPrefixForType(SessionType type)
	{
		switch(type)
		{
			case HttpSession: return "zhttp";
			case WebSocketSession: return "zws";
			case CacheRequest: return "cache-request";
			case CacheResponse: return "cache-response";
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
		if (gCacheEnable == true)
			writeToClient(type, zresp, zhttpAddress);
		else
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
				if(p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel)
					send_response_to_client(ZhttpResponsePacket::Cancel, id.id, p.from);
				return;
			}

			if (gCacheEnable == true)
			{
				pause_cache_thread();

				// if requests from cache client
				int ccIndex = get_cc_index_from_init_request(p);
				if (ccIndex >= 0 && ccIndex < gWsCacheClientList.count())
				{
					gWsCacheClientList[ccIndex].initFlag = false;
					gWsCacheClientList[ccIndex].clientId = id.id;
					gWsCacheClientList[ccIndex].instanceId = instanceId;
					gWsCacheClientList[ccIndex].msgIdCount = -1;
					gWsCacheClientList[ccIndex].from = p.from;
					gWsCacheClientList[ccIndex].lastRequestSeq = id.seq;
					gWsCacheClientList[ccIndex].lastResponseSeq = -1;
					gWsCacheClientList[ccIndex].lastRequestTime = QDateTime::currentMSecsSinceEpoch();

					log_debug("[WS] Registered new cache %d client=%s, from=%s, instanceId=%s", 
						ccIndex, id.id.constData(), p.from.constData(), instanceId.constData());
				}
				else // if request from real client
				{
					log_debug("[WS] received init request from real client");
					if (get_main_cc_index(instanceId) < 0)
					{
						log_warning("[WS] not initialized cache client, ignore");
						if(p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel)
							send_response_to_client(ZhttpResponsePacket::Cancel, id.id, p.from);
						resume_cache_thread();
						return;
					}
					else
					{
						// get resp key
						QByteArray responseKey = calculate_response_seckey_from_init_request(p);
						// register ws client
						register_ws_client(id.id, p.from, p.uri.toString());
						// respond with cached init packet
						send_response_to_client(ZhttpResponsePacket::Data, id.id, p.from, 0, &gWsInitResponsePacket, responseKey);
						resume_cache_thread();
						return;
					}
				}

				resume_cache_thread();
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
				if(p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel)
					send_response_to_client(ZhttpResponsePacket::Cancel, id.id, p.from);
				return;
			}

			// cache process
			if (gCacheEnable == true)
			{
				pause_cache_thread();

				if (!p.headers.contains(HTTP_REFRESH_HEADER))
				{
					register_http_client(id.id, p.from, p.uri.toString());
				}
				else
				{
					QString tmpStr = QString::fromUtf8(p.headers.get(HTTP_REFRESH_HEADER));
					QByteArray msgIdByte = QByteArray::fromHex(qPrintable(tmpStr.remove('\"')));
					CacheItem *pCacheItem = load_cache_item(msgIdByte);
					if (pCacheItem != NULL)
					{
						pCacheItem->requestPacket.ids[0].id = id.id;
						//store_cache_item_field(msgIdByte, "requestPacket", TnetString::fromVariant(pCacheItem->requestPacket.toVariant()));
					}
					// remove HTTP_REFRESH_HEADER header
					p.headers.removeAll(HTTP_REFRESH_HEADER);
				}

				resume_cache_thread();
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
			if(p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel)
				send_response_to_client(ZhttpResponsePacket::Cancel, id.id, p.from);
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

		for	(int i=0; i<p.ids.count(); i++)
		{
			QByteArray packetId = p.ids[i].id;
			int newSeq = p.ids[i].seq;

			// cache process
			if (gCacheEnable == true)
			{
				pause_cache_thread();

				// complete tasks from cache thread
				send_unsubscribe_request_over_cacheclient();
				delete_old_clients();

				if (gReplicaFlag == true && gReplicaTimerStarted == false)
				{
					gReplicaTimerStarted = true;
					QTimer::singleShot(1 * 1000, [=]() {
						scan_subscribe_update();
					});
				}

				// if request from cache client, skip
				if (gHttpClientMap.contains(packetId))
				{
					log_debug("[HTTP] received http request from real client=%s", packetId.data());

					// update client last request time
					gHttpClientMap[packetId].lastRequestTime = QDateTime::currentMSecsSinceEpoch();

					// if cancel/close request, remove client from the subscription client list
					int ret;
					switch (p.type)
					{
					case ZhttpRequestPacket::Cancel:
						unregister_client(packetId);
						//send_wsCloseResponse(packetId);
						break;
					case ZhttpRequestPacket::Close:
						unregister_client(packetId);
						break;
					case ZhttpRequestPacket::Data:
						ret = process_http_request(packetId, p, gHttpClientMap[packetId].urlPath);
						if (ret == 0)
						{
							p.type = ZhttpRequestPacket::Cancel;
							//resume_cache_thread();
							//continue;
						}
						break;
					default:
						break;
					}
				}
				else if (gWsClientMap.contains(packetId))
				{
					log_debug("[WS] received ws request from real client=%s", packetId.data());

					// update client last request time
					gWsClientMap[packetId].lastRequestTime = QDateTime::currentMSecsSinceEpoch();

					// if cancel/close request, remove client from the subscription client list
					switch (p.type)
					{
					case ZhttpRequestPacket::Cancel:
						unregister_client(packetId);
						//send_wsCloseResponse(packetId);
						break;
					case ZhttpRequestPacket::Close:
						send_response_to_client(ZhttpResponsePacket::Close, packetId, p.from);
						unregister_client(packetId);
						break;
					case ZhttpRequestPacket::KeepAlive:
						log_debug("[WS] received KeepAlive, ignoring");
						//send_pingResponse(packetId);
						break;
					case ZhttpRequestPacket::Pong:
						send_response_to_client(ZhttpResponsePacket::Credit, packetId, p.from, 0);
						break;
					case ZhttpRequestPacket::Ping:
						send_response_to_client(ZhttpResponsePacket::Pong, packetId, p.from);
						break;
					case ZhttpRequestPacket::Credit:
						log_debug("[WS] received Credit, ignoring");
						break;
					case ZhttpRequestPacket::Data:
						// Send new credit packet
						send_response_to_client(ZhttpResponsePacket::Credit, packetId, p.from, static_cast<int>(p.body.size()));
						process_ws_stream_request(packetId, p);
						break;
					default:
						break;
					}

					resume_cache_thread();
					continue;
				}
				else // cache client
				{
					switch (p.type)
					{
					case ZhttpRequestPacket::Cancel:
					case ZhttpRequestPacket::Close:
					case ZhttpRequestPacket::Error:
						{
							log_debug("[WS] switching client of request error, condition=%s", p.condition.data());

							// get error type
							QString conditionStr = QString(p.condition);
							if (conditionStr.compare("remote-connection-failed", Qt::CaseInsensitive) == 0 ||
								conditionStr.compare("connection-timeout", Qt::CaseInsensitive) == 0)
							{
								log_debug("[WS] Sleeping for 10 seconds");
								sleep(10);
							}

							// if cache client0 is ON, start cache client1
							int ccIndex = get_cc_index_from_clientId(packetId);
							if (ccIndex >= 0)
							{
								log_debug("[WS] disabled cache client %d", ccIndex);
								QString urlPath = gWsBackendUrlList[ccIndex];
								wsCacheClientConnectFailedCountMap[urlPath]++;
								gWsCacheClientList[ccIndex].initFlag = false;
							}
						}
						break;
					default:
						break;
					}
				}
				
				resume_cache_thread();

				newSeq = update_request_seq(packetId);
				if (newSeq >= 0)
					p.ids[i].seq = newSeq;
				else
					newSeq = p.ids[i].seq;
			}

			// is this for a websocket?
			ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(p.from, packetId));
			if(sock)
			{
				sock->handle(packetId, newSeq, p);
				if(self.expired())
					return;

				continue;
			}

			// is this for an http request?
			ZhttpRequest *req = serverReqsByRid.value(ZhttpRequest::Rid(p.from, packetId));
			if(req)
			{
				req->handle(packetId, newSeq, p);
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

	void timer_send_ping_to_client(QByteArray clientId)
	{
		if (gWsClientMap.contains(clientId))
		{
			log_debug("_[TIMER] send ping to client %s", clientId.data());
			send_response_to_client(ZhttpResponsePacket::Ping, clientId, gWsClientMap[clientId].from);
			QTimer::singleShot(PING_INTERVAL * 1000, [=]() {
				timer_send_ping_to_client(clientId);
			});
		}
		else
		{
			log_debug("_[TIMER] exit ping for client %s", clientId.data());
		}
	}

	void refresh_cache(QByteArray itemId, QString urlPath, qint64 refreshCount)
	{
		log_debug("_[TIMER] cache refresh %s %s", itemId.toHex().data(), qPrintable(urlPath));
		CacheItem *pCacheItem = load_cache_item(itemId);
		if (pCacheItem == NULL)
		{
			log_debug("_[TIMER] exit refresh %s", itemId.toHex().data());
			return;
		}

		// check refresh count (can be duplicated by the removed cache item.)
		if (refreshCount != pCacheItem->lastRefreshCount)
		{
			log_debug("_[TIMER] got invalid timer %s, expect %d, but %d", itemId.toHex().data(), refreshCount, pCacheItem->lastRefreshCount);
			return;
		}
		pCacheItem->lastRefreshCount++;
		refreshCount = pCacheItem->lastRefreshCount;

		int timeInterval = get_next_cache_refresh_interval(itemId);
		qint64 currMTime = QDateTime::currentMSecsSinceEpoch();
		qint64 accessTimeoutMSeconds = gAccessTimeoutSeconds * 1000;
		if (pCacheItem->cachedFlag == true)
		{
			// delete old cache items if it`s not auto_refresh_unerase
			if ((pCacheItem->refreshFlag & AUTO_REFRESH_UNERASE) == 0)
			{
				qint64 accessDiff = currMTime - pCacheItem->lastAccessTime;
				if (accessDiff > accessTimeoutMSeconds)
				{
					// remove cache item
					log_debug("[CACHE] deleting cache item for access timeout %s", itemId.toHex().data());
					remove_cache_item(itemId);
					return;
				}
			}

			if (timeInterval > 0)
			{
				if (pCacheItem->proto == Scheme::http)
				{
					QByteArray reqBody = pCacheItem->requestPacket.body;
					QString newMsgId = QString("\"%1\"").arg(itemId.toHex().data());
					replace_id_field(reqBody, QString("__MSGID__"), newMsgId);
					send_http_post_request_with_refresh_header(urlPath, reqBody, itemId.toHex().data());
				}
				else if (pCacheItem->proto == Scheme::websocket)
				{
					// Send client cache request packet for auto-refresh
					int ccIndex = get_cc_index_from_clientId(pCacheItem->cacheClientId);
					pCacheItem->newMsgId = send_ws_request_over_cacheclient(pCacheItem->requestPacket, QString("__MSGID__"), ccIndex);
					pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
				}
			}
		}
		else
		{
			if (pCacheItem->retryCount > RETRY_RESPONSE_MAX_COUNT)
			{
				log_debug("[_TIMER] reached max retry count");
				return;
			}
			pCacheItem->retryCount++;
			// switch backend of the failed response
			if (pCacheItem->proto == Scheme::http)
			{
				qint64 accessDiff = currMTime - pCacheItem->lastAccessTime;
				if (accessDiff > accessTimeoutMSeconds)
				{
					// prometheus status
					if (httpCacheClientConnectFailedCountMap.contains(urlPath))
						httpCacheClientConnectFailedCountMap[urlPath]++;
				}

				urlPath = get_switched_http_backend_url(urlPath);
				for (int i=0; i<gHttpBackendUrlList.count(); i++)
				{
					if (gHttpBackendUrlList[i] == urlPath)
					{
						pCacheItem->httpBackendNo = i;
						//store_cache_item_field(itemId, "httpBackendNo", pCacheItem->httpBackendNo);
						break;
					}
				}

				QByteArray reqBody = pCacheItem->requestPacket.body;
				QString newMsgId = QString("\"%1\"").arg(itemId.toHex().data());
				replace_id_field(reqBody, QString("__MSGID__"), newMsgId);
				send_http_post_request_with_refresh_header(urlPath, reqBody, itemId.toHex().data());
			}
			else if (pCacheItem->proto == Scheme::websocket)
			{
				qint64 accessDiff = currMTime - pCacheItem->lastAccessTime;
				if (accessDiff > accessTimeoutMSeconds)
				{
					// prometheus status
					if (wsCacheClientConnectFailedCountMap.contains(urlPath))
						wsCacheClientConnectFailedCountMap[urlPath]++;
				}

				// Send client cache request packet for auto-refresh
				int ccIndex = get_cc_next_index_from_clientId(pCacheItem->cacheClientId, instanceId);
				pCacheItem->cacheClientId = gWsCacheClientList[ccIndex].clientId;
				urlPath = gWsCacheClientList[ccIndex].urlPath;
				pCacheItem->newMsgId = send_ws_request_over_cacheclient(pCacheItem->requestPacket, QString("__MSGID__"), ccIndex);
				pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
			}
		}

		if (timeInterval > 0)
		{
			QTimer::singleShot(timeInterval * 1000, [=]() {
				refresh_cache(itemId, urlPath, refreshCount);
			});
		}
	}

	void register_cache_refresh(QByteArray itemId, QString urlPath)
	{
		CacheItem *pCacheItem = load_cache_item(itemId);
		if (pCacheItem == NULL)
		{
			log_debug("[REFRESH] Canceled cache item because it not exist %s", itemId.toHex().data());
			return;
		}

		log_debug("[REFRESH] Registered new cache refresh %s, %s", itemId.toHex().data(), qPrintable(urlPath));

		pCacheItem->lastRefreshCount = 0;
		int timeInterval = get_next_cache_refresh_interval(itemId);
		if (timeInterval > 0)
		{
			QTimer::singleShot(timeInterval * 1000, [=]() {
				refresh_cache(itemId, urlPath, 0);
			});
		}
		//store_cache_item_field(itemId, "lastRefreshCount", 0);
	}

	void scan_subscribe_update()
	{
		log_debug("[SCAN]");
		foreach(QByteArray itemId, get_cache_item_ids())
		{
			CacheItem* pCacheItem = load_cache_item(itemId);
			if (pCacheItem == NULL)
			{
				log_debug("[SCAN] not found cache item %s", itemId.toHex().data());
				continue;
			}
			if ((pCacheItem->cachedFlag == true) && (pCacheItem->proto == Scheme::none) && 
				(pCacheItem->methodType == SUBSCRIBE_METHOD))
			{
				QByteArray updateCountKey = itemId + "-updateCount";
				QByteArray countBytes = redis_load_cache_response(updateCountKey);
				int updateCount = countBytes.toInt();
				log_debug("[SCAN] %d, %d", pCacheItem->subscriptionUpdateCount, updateCount);
				if (pCacheItem->subscriptionUpdateCount != updateCount)
				{
					pCacheItem->subscriptionUpdateCount = updateCount;
					pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();

					QByteArray updateKey = itemId + "-update";
					QByteArray packetBuf = redis_load_cache_response(updateKey);
					QVariant data = TnetString::toVariant(packetBuf);
					if(!data.isNull())
					{
						ZhttpResponsePacket p;
						if(p.fromVariant(data))
						{
							log_debug("[SUBSCRIBE] sending update to replica");
							// send update subscribe to all clients
							QHash<QByteArray, ClientInCacheItem>::iterator it = pCacheItem->clientMap.begin();
							while (it != pCacheItem->clientMap.end()) 
							{
								QByteArray cliId = it.key();
								if (gWsClientMap.contains(cliId))
								{
									QString clientMsgId = pCacheItem->clientMap[cliId].msgId;
									QByteArray clientInstanceId = pCacheItem->clientMap[cliId].instanceId;
									QByteArray clientFrom = pCacheItem->clientMap[cliId].from;

									log_debug("[SUBSCRIBE] Sending Subscription update to client id=%s, msgId=%s, instanceId=%s", 
											cliId.data(), qPrintable(clientMsgId), clientInstanceId.data());

									writeToClient__(CacheResponse, p, cliId, clientFrom, clientInstanceId);

									++it;
								}
								else 
								{
									it = pCacheItem->clientMap.erase(it);  // erase returns the next valid iterator
								}
							}
						}
					}
				}
			}
		}

		QTimer::singleShot(1 * 1000, [=]() {
			scan_subscribe_update();
		});
	}

	void unregister_client(const QByteArray& clientId)
	{
		if (gHttpClientMap.contains(clientId))
		{
			gHttpClientMap.remove(clientId);
			log_debug("[HTTP] Deleted http client=%s", clientId.data());
		}
		else
		{
			// delete client from gWsClientMap
			gWsClientMap.remove(clientId);
			log_debug("[WS] Deleted ws client=%s", clientId.data());
		}
	}

	void register_http_client(QByteArray packetId, QByteArray from, QString urlPath)
	{
		if (gHttpClientMap.contains(packetId))
		{
			log_debug("[HTTP] already exists http client id=%s", packetId.data());
			return;
		}

		struct ClientItem clientItem;
		clientItem.lastRequestSeq = 0;
		clientItem.lastResponseSeq = -1;
		clientItem.lastRequestTime = QDateTime::currentMSecsSinceEpoch();
		clientItem.lastResponseTime = QDateTime::currentMSecsSinceEpoch();
		clientItem.from = from;
		clientItem.urlPath = urlPath;
		gHttpClientMap[packetId] = clientItem;
		log_debug("[HTTP] added http client id=%s", packetId.data());

		return;
	}

	void register_ws_client(QByteArray packetId, QByteArray from, QString urlPath)
	{
		if (gWsClientMap.contains(packetId))
		{
			log_debug("[WS] already exists ws client id=%s", packetId.data());
			return;
		}

		struct ClientItem clientItem;
		clientItem.lastRequestSeq = 0;
		clientItem.lastResponseSeq = -1;
		clientItem.lastRequestTime = QDateTime::currentMSecsSinceEpoch();
		clientItem.lastResponseTime = QDateTime::currentMSecsSinceEpoch();
		clientItem.from = from;
		clientItem.urlPath = urlPath;
		gWsClientMap[packetId] = clientItem;
		log_debug("[WS] added ws client id=%s", packetId.data());

		QTimer::singleShot(PING_INTERVAL * 1000, [=]() {
			timer_send_ping_to_client(packetId);
		});

		return;
	}

	void register_http_cache_item(
		const ZhttpRequestPacket &clientPacket, 
		QByteArray clientId, 
		const PacketMsg &packetMsg, 
		int backendNo)
	{
		// create new cache item
		struct CacheItem cacheItem;
		cacheItem.newMsgId = -1;
		cacheItem.refreshFlag = 0x00;
		if (is_never_timeout_method(packetMsg.method, packetMsg.params))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_NEVER_TIMEOUT;
			log_debug("[HTTP] added refresh never timeout method");
		}
		if (gRefreshShorterMethodList.contains(packetMsg.method, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_SHORTER_TIMEOUT;
			log_debug("[HTTP] added refresh shorter method");
		}
		if (gRefreshLongerMethodList.contains(packetMsg.method, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_LONGER_TIMEOUT;
			log_debug("[HTTP] added refresh longer method");
		}
		if (gRefreshUneraseMethodList.contains(packetMsg.method, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_UNERASE;
			log_debug("[HTTP] added refresh unerase method");
		}
		if (gRefreshExcludeMethodList.contains(packetMsg.method, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_EXCLUDE;
			log_debug("[HTTP] added refresh exclude method");
		}
		if (gRefreshPassthroughMethodList.contains(packetMsg.method, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_PASSTHROUGH;
			log_debug("[HTTP] added refresh passthrough method");
		}
		if (gNullResponseMethodList.contains(packetMsg.method, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= ACCEPT_NULL_RESPONSE;
			log_debug("[HTTP] added null response method");
		}
		cacheItem.lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.lastRefreshCount = 0;
		cacheItem.lastAccessTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.lastRequestTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.cachedFlag = false;

		cacheItem.methodName = packetMsg.method;

		// check cache/subscribe method
		if (is_cache_method(packetMsg.method))
		{
			cacheItem.methodType = CACHE_METHOD;
		}
		else if (is_subscribe_method(packetMsg.method))
		{
			cacheItem.methodType = SUBSCRIBE_METHOD;
		}

		// save the request packet with new id
		cacheItem.requestPacket = clientPacket;
		replace_id_field(cacheItem.requestPacket.body, packetMsg.id, QString("__MSGID__"));
		cacheItem.clientMap[clientId].msgId = packetMsg.id;
		cacheItem.clientMap[clientId].from = clientPacket.from;
		cacheItem.clientMap[clientId].instanceId = instanceId;
		cacheItem.proto = Scheme::http;
		cacheItem.retryCount = 0;
		cacheItem.httpBackendNo = backendNo;

		create_cache_item(packetMsg.paramsHash, cacheItem);

		log_debug("[HTTP] Registered New Cache Item for id=%s method=\"%s\" backend=%d", qPrintable(packetMsg.id), qPrintable(packetMsg.method), backendNo);
	}

	int register_ws_cache_item(
		const ZhttpRequestPacket &clientPacket, 
		QByteArray clientId, 
		QString orgMsgId, 
		QString methodName,
		QString msgParams, 
		const QByteArray &methodNameParamsHashVal)
	{
		// create new cache item
		struct CacheItem cacheItem;

		int ccIndex = get_main_cc_index(instanceId);
		if (ccIndex < 0)
			return -1;
		cacheItem.newMsgId = gWsCacheClientList[ccIndex].msgIdCount;
		cacheItem.refreshFlag = 0x00;
		if (is_never_timeout_method(methodName, msgParams))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_NEVER_TIMEOUT;
			log_debug("[WS] added refresh never timeout method");
		}
		if (gRefreshShorterMethodList.contains(methodName, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_SHORTER_TIMEOUT;
			log_debug("[WS] added refresh shorter method");
		}
		if (gRefreshLongerMethodList.contains(methodName, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_LONGER_TIMEOUT;
			log_debug("[WS] added refresh longer method");
		}
		if (gRefreshUneraseMethodList.contains(methodName, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_UNERASE;
			log_debug("[WS] added refresh unerase method");
		}
		if (gRefreshExcludeMethodList.contains(methodName, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_EXCLUDE;
			log_debug("[WS] added refresh exclude method");
		}
		if (gRefreshPassthroughMethodList.contains(methodName, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= AUTO_REFRESH_PASSTHROUGH;
			log_debug("[WS] added refresh passthrough method");
		}
		if (gNullResponseMethodList.contains(methodName, Qt::CaseInsensitive))
		{
			cacheItem.refreshFlag |= ACCEPT_NULL_RESPONSE;
			log_debug("[WS] added null response method");
		}
		cacheItem.lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.lastRefreshCount = 0;
		cacheItem.lastAccessTime = QDateTime::currentMSecsSinceEpoch();
		cacheItem.cachedFlag = false;

		// save the request packet with new id
		cacheItem.requestPacket = clientPacket;
		replace_id_field(cacheItem.requestPacket.body, orgMsgId, QString("__MSGID__"));
		cacheItem.clientMap[clientId].msgId = orgMsgId;
		cacheItem.clientMap[clientId].from = clientPacket.from;
		cacheItem.clientMap[clientId].instanceId = instanceId;
		cacheItem.proto = Scheme::websocket;
		cacheItem.retryCount = 0;
		cacheItem.cacheClientId = gWsCacheClientList[ccIndex].clientId;
		cacheItem.methodName = methodName;
		cacheItem.subscriptionUpdateCount = 0;

		// check cache/subscribe method
		if (is_cache_method(methodName))
		{
			cacheItem.methodType = CACHE_METHOD;
		}
		else if (is_subscribe_method(methodName))
		{
			cacheItem.methodType = SUBSCRIBE_METHOD;
		}

		create_cache_item(methodNameParamsHashVal, cacheItem);

		return ccIndex;
	}

	int process_http_request(QByteArray id, const ZhttpRequestPacket &p, const QString &urlPath)
	{
		QByteArray packetId = id;

		// parse json body
		PacketMsg packetMsg;
		if (parse_packet_msg(Scheme::http, p, packetMsg, instanceId) < 0)
			return -1;

		// get method string
		if (packetMsg.id.isEmpty() || packetMsg.method.isEmpty())
		{
			log_debug("[HTTP] failed to get gMsgIdAttrName and gMsgMethodAttrName");
			return -1;
		}
		log_debug("[HTTP] new req msgId=%s method=%s msgParams=%s", 
			qPrintable(packetMsg.id), qPrintable(packetMsg.method), qPrintable(packetMsg.params));

		// update the counter for prometheus
		gCacheMethodRequestCountList.append(packetMsg.method);

		if (is_cache_method(packetMsg.method))
		{
			CacheItem *pCacheItem = load_cache_item(packetMsg.paramsHash);
			if (pCacheItem != NULL)
			{
				pCacheItem->lastAccessTime = QDateTime::currentMSecsSinceEpoch();

				// prometheus staus
				update_prometheus_hit_count(*pCacheItem);

				// send credit response to client
				send_response_to_client(ZhttpResponsePacket::Credit, packetId, p.from, static_cast<int>(p.body.size()));

				if (pCacheItem->cachedFlag == true)
				{
					writeToClient_(packetMsg.paramsHash, packetId, packetMsg.id, p.from, instanceId);
					log_debug("[HTTP] Replied with Cache content for method \"%s\"", qPrintable(packetMsg.method));
					unregister_client(packetId);
				}
				else
				{
					log_debug("[HTTP] Already cache registered, but not added content \"%s\"", qPrintable(packetMsg.method));
					// add client to list
					pCacheItem->clientMap[packetId].msgId = packetMsg.id;
					pCacheItem->clientMap[packetId].from = p.from;
					pCacheItem->clientMap[packetId].instanceId = instanceId;
					log_debug("[HTTP] Adding new client id msgId=%s clientId=%s", qPrintable(packetMsg.id), packetId.data());
					pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
				}

				return 0;
			}
			else
			{
				log_debug("[HTTP] not found in cache");
			}

			int backendNo = -1;
			for (int i = 0; i < gHttpBackendUrlList.count(); i++)
			{
				if (urlPath == gHttpBackendUrlList[i])
				{
					backendNo = i;
					break;
				}				
			}

			// Register new cache item
			register_http_cache_item(p, packetId, packetMsg, backendNo);

			// register cache refresh
			register_cache_refresh(packetMsg.paramsHash, urlPath);
		}

		return -1;
	}

	int process_http_response(const ZhttpResponsePacket &responsePacket, const QByteArray &instanceAddress)
	{
		ZhttpResponsePacket p = responsePacket;
		QByteArray packetId = p.ids[0].id;
		QByteArray from = p.from;

		// check multi-part response
		int ret = check_multi_packets_for_http_response(p);
		if (ret < 0)
			return 0;

		QVariant vpacket = p.toVariant();
		QByteArray responseBuf = instanceAddress + " T" + TnetString::fromVariant(vpacket);
		
		bool bodyParseSucceed = true;

		// parse json body
		PacketMsg packetMsg;
		if (parse_packet_msg(Scheme::http, p, packetMsg, instanceId) < 0)
			bodyParseSucceed = false;

		CacheItem *pCacheItem = NULL;
		QByteArray itemId = QByteArray();
		if (bodyParseSucceed == true)
		{
			// convert to QByteArray
			QString tmpStr = packetMsg.id;
			QByteArray msgIdByte = QByteArray::fromHex(qPrintable(tmpStr.remove('\"')));

			pCacheItem = load_cache_item(msgIdByte);
			if (pCacheItem != NULL)
			{
				itemId = msgIdByte;
			}
		}

		if (itemId.isEmpty())
		{
			foreach(QByteArray _itemId, get_cache_item_ids())
			{
				pCacheItem = load_cache_item(_itemId);
				if (pCacheItem == NULL)
				{
					log_debug("[HTTP] not found cache item %s", _itemId.toHex().data());
					continue;
				}
				if ((pCacheItem->proto == Scheme::http) && 
					(pCacheItem->requestPacket.ids[0].id == packetId) &&
					(pCacheItem->cachedFlag == false)
				)
				{
					itemId = _itemId;
					break;
				}
			}
		}

		if (itemId.isEmpty() || pCacheItem->proto != Scheme::http)
		{
			log_debug("[HTTP] could not find the cache item %s", itemId.constData());
			return -1;
		}

		// if invalid response?
		if ((bodyParseSucceed == false || (pCacheItem->cachedFlag == false && packetMsg.isResultNull == true)) &&
			!(pCacheItem->refreshFlag & ACCEPT_NULL_RESPONSE) && 
			pCacheItem->retryCount < RETRY_RESPONSE_MAX_COUNT)
		{
			// prometheus status
			if (pCacheItem->httpBackendNo >= 0)
			{
				QString urlPath = gHttpBackendUrlList[pCacheItem->httpBackendNo];
				if (httpCacheClientInvalidResponseCountMap.contains(urlPath))
					httpCacheClientInvalidResponseCountMap[urlPath]++;
			}
			
			log_debug("[HTTP] get NULL response, retrying %d", pCacheItem->retryCount);
			pCacheItem->lastAccessTime = QDateTime::currentMSecsSinceEpoch();

			//store_cache_item_field(msgIdByte, "lastAccessTime", pCacheItem->lastAccessTime);
			return 0;
		}

		// send response to clients.
		pCacheItem->newMsgId = 0;
		pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
		pCacheItem->cachedFlag = true;
		log_debug("[HTTP] Added/Updated Cache content for method=%s", qPrintable(pCacheItem->methodName));

		// store response body
		store_cache_response_buffer(itemId, responseBuf, packetMsg.id, 0);

		foreach(QByteArray cliId, pCacheItem->clientMap.keys())
		{
			if (gHttpClientMap.contains(cliId))
			{
				QString msgId = pCacheItem->clientMap[cliId].msgId;
				QByteArray orgInstanceId = pCacheItem->clientMap[cliId].instanceId;
				writeToClient_(itemId, cliId, msgId, instanceAddress, orgInstanceId);

				log_debug("[HTTP] Sent Cache content to client id=%s", cliId.data());
				unregister_client(cliId);
			}
		}
		pCacheItem->clientMap.clear();

		return 0;
	}

	int process_ws_cacheclient_response(const ZhttpResponsePacket &response, int cacheClientNumber, const QByteArray &instanceAddress)
	{
		ZhttpResponsePacket p = response;
		
		// check multi-part response
		int ret = check_multi_packets_for_ws_response(p);
		if (ret < 0)
			return -1;

		QByteArray packetId = p.ids[0].id;

		QVariant vpacket = p.toVariant();
		QByteArray packetBuf = TnetString::fromVariant(vpacket);
		QByteArray responseBuf = instanceAddress + " T" + packetBuf;

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s server: OUT %s", "[CacheClient]", instanceAddress.data());

		// parse json body
		PacketMsg packetMsg;
		if (parse_packet_msg(Scheme::websocket, p, packetMsg, instanceId) < 0)
			return -1;

		// id
		int msgIdValue = is_convertible_to_int(packetMsg.id) ? packetMsg.id.toInt() : -1;
		
		// result
		QString msgResultStr = packetMsg.result;

		// if it is curie response without change, ignore			
		QString methodName = packetMsg.method;

		if (!packetMsg.subscription.isEmpty())
		{
			QString subscriptionStr = packetMsg.subscription;

			foreach(QByteArray itemId, get_cache_item_ids())
			{
				CacheItem* pCacheItem = load_cache_item(itemId);
				if (pCacheItem == NULL)
				{
					log_debug("[WS] not found cache item %s", itemId.toHex().data());
					continue;
				}
				if (pCacheItem->subscriptionStr == subscriptionStr)
				{
					if (pCacheItem->cachedFlag == false)
					{
						pCacheItem->cachedFlag = true;
						pCacheItem->subscriptionUpdateCount = 0;

						// update block and changes
						if (!packetMsg.resultBlock.isEmpty())
						{
							QString msgBlockStr = packetMsg.resultBlock.toLower();
							pCacheItem->blockStr = msgBlockStr;
						}

						if (!packetMsg.resultChanges.isEmpty())
						{
							QString msgChangesStr = packetMsg.resultChanges.toLower();

							QStringList changesList = msgChangesStr.split("/");
							for ( const auto& changes : changesList )
							{
								QStringList changeList = changes.split("+");
								if (changeList.size() != 2)
								{
									log_debug("[WS] Invalid change list");
									continue;
								}
								pCacheItem->changesMap[changeList[0]] = changeList[1];
							}
						}

						// store response body
						QByteArray subscriptionKey = itemId + "-sub";
						store_cache_response_buffer(subscriptionKey, responseBuf, QString(""), 0);

						log_debug("[WS] Added Subscription content for subscription method id=%d subscription=%s", 
							pCacheItem->newMsgId, qPrintable(subscriptionStr));
						// send update subscribe to all clients
						QHash<QByteArray, ClientInCacheItem>::iterator it = pCacheItem->clientMap.begin();
						while (it != pCacheItem->clientMap.end()) 
						{
							QByteArray cliId = it.key();
							if (gWsClientMap.contains(cliId))
							{
								QString clientMsgId = pCacheItem->clientMap[cliId].msgId;
								QByteArray clientInstanceId = pCacheItem->clientMap[cliId].instanceId;

								log_debug("[WS] Sending Subscription content to client id=%s, msgId=%s, instanceId=%s", 
										cliId.data(), qPrintable(clientMsgId), clientInstanceId.data());
								
								writeToClient_(itemId, cliId, clientMsgId, instanceAddress, clientInstanceId);
								writeToClient_(subscriptionKey, cliId, clientMsgId, instanceAddress, clientInstanceId);

								++it;
							}
							else 
							{
								it = pCacheItem->clientMap.erase(it);  // erase returns the next valid iterator
							}
						}
					}
					else
					{
						QByteArray subscriptionKey = itemId + "-sub";
						if (!packetMsg.resultBlock.isEmpty() || !packetMsg.resultChanges.isEmpty())
						{
							QByteArray responseBuf_ = load_cache_response_buffer(instanceAddress, subscriptionKey, packetId, 0, QString("__ID__"), "__FROM__", 0);

							int diffLen = 0;
							
							// update block and changes
							if (!packetMsg.resultBlock.isEmpty())
							{
								QString newBlockStr = packetMsg.resultBlock.toLower();
								QString oldBlckStr = pCacheItem->blockStr;

								QByteArray oldPatternStr = "\"block\":";
								oldPatternStr += "\"";
								oldPatternStr += oldBlckStr.toUtf8();
								oldPatternStr += "\"";
								QByteArray newPatternStr = "\"block\":";
								newPatternStr += "\"";
								newPatternStr += newBlockStr.toUtf8();
								newPatternStr += "\"";

								qsizetype idxStart = responseBuf_.indexOf(oldPatternStr);
								if (idxStart >= 0)
								{
									log_debug("[PPP-1] %s,%s", oldPatternStr.constData(), newPatternStr.constData());
									responseBuf_.replace(idxStart, oldPatternStr.length(), newPatternStr);
									diffLen += newPatternStr.length() - oldPatternStr.length();
									pCacheItem->blockStr = newBlockStr;
								}
							}
							///*
							if (!packetMsg.resultChanges.isEmpty())
							{
								QString msgChangesStr = packetMsg.resultChanges.toLower();

								QStringList changesList = msgChangesStr.split("/");
								for ( const auto& changes : changesList )
								{
									QStringList changeList = changes.split("+");
									if (changeList.size() != 2)
									{
										log_debug("[WS] Invalid change list");
										continue;
									}

									QString changesKey = changeList[0];
									QString oldVal = pCacheItem->changesMap[changeList[0]];
									QString newVal = changeList[1];

									QByteArray oldPatternStr = "[\"";
									oldPatternStr += changesKey.toUtf8();
									oldPatternStr += (oldVal != "null") ? "\",\"" : "\",";
									oldPatternStr += oldVal.toUtf8();
									oldPatternStr += (oldVal != "null") ? "\"]" : "]";
									QByteArray newPatternStr = "[\"";
									newPatternStr += changesKey.toUtf8();
									newPatternStr += (newVal != "null") ? "\",\"" : "\",";
									newPatternStr += newVal.toUtf8();
									newPatternStr += (newVal != "null") ? "\"]" : "]";

									qsizetype idxStart = responseBuf_.indexOf(oldPatternStr);
									if (idxStart >= 0)
									{
										log_debug("[PPP-2] %s,%s", oldPatternStr.constData(), newPatternStr.constData());
										responseBuf_.replace(idxStart, oldPatternStr.length(), newPatternStr);
										diffLen += newPatternStr.length() - oldPatternStr.length();
										pCacheItem->changesMap[changesKey] = newVal;
									}
								}
							}
							//*/
							
							/*
							QByteArray patternStr = "\"block\":\"";
							qsizetype idxStart = responseBuf_.indexOf(patternStr);
							if (idxStart >= 0)
							{
								qsizetype idxEnd = responseBuf_.indexOf("\"", idxStart+9);
								responseBuf_.replace(idxStart+9, idxEnd-(idxStart+9), QByteArray(qPrintable(msgBlockStr)));
							}
							else
							{
								log_debug("[WS] not found block in subscription cached response");
							}

							QString msgChangesStr = packetMsg.resultChanges.toLower();
							QStringList changesList = msgChangesStr.split("/");
							for ( const auto& changes : changesList )
							{
								QStringList changeList = changes.split("+");
								if (changeList.size() != 2)
								{
									log_debug("[WS] Invalid change list");
									continue;
								}

								log_debug("[QQQ] %s,%s", qPrintable(changeList[0]), qPrintable(changeList[1]));

								QString patternStr(qPrintable("[\"" + changeList[0] + "\""));
								QString newPattern = "[\"";
								newPattern += changeList[0];
								newPattern += "\",\"";
								newPattern += changeList[1];
								newPattern += "\"]";

								qsizetype idxStart = 0;
								qsizetype idxEnd = 0;
								while (1)
								{
									idxStart = responseBuf_.indexOf(patternStr, idxEnd);
									if (idxStart < 0)
										break;
									
									idxEnd = responseBuf_.indexOf("]", idxStart+changeList[0].length());
									if (idxEnd > idxStart)
									{
										responseBuf_.replace(idxStart, idxEnd-idxStart+1, qPrintable(newPattern));
										diffLen += newPattern.length() - (idxEnd-idxStart+1);
									}
									else
									{
										log_debug("[WS] not found change param in subscription cached response");
										break;
									}	
								}
							}
							*/

							// store response body
							store_cache_response_buffer(subscriptionKey, responseBuf_, QString(""), diffLen);
						}
						else
						{
							// store response body
							store_cache_response_buffer(subscriptionKey, responseBuf, QString(""), 0);
						}

						// update subscription last update time
						pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();

						// save update packet into redis
						if (gRedisEnable == true && gReplicaFlag == false)
						{
							QByteArray updateKey = itemId + "-update";
							redis_store_cache_response(updateKey, packetBuf);

							QByteArray updateCountKey = itemId + "-updateCount";
							pCacheItem->subscriptionUpdateCount++;
							QByteArray countBytes = QByteArray::number(pCacheItem->subscriptionUpdateCount);
							redis_store_cache_response(updateCountKey, countBytes);
						}

						// send update subscribe to all clients
						QHash<QByteArray, ClientInCacheItem>::iterator it = pCacheItem->clientMap.begin();
						while (it != pCacheItem->clientMap.end()) 
						{
							QByteArray cliId = it.key();
							if (gWsClientMap.contains(cliId))
							{
								QString clientMsgId = pCacheItem->clientMap[cliId].msgId;
								QByteArray clientInstanceId = pCacheItem->clientMap[cliId].instanceId;

								log_debug("[WS] Sending Subscription update to client id=%s, msgId=%s, instanceId=%s", 
										cliId.data(), qPrintable(clientMsgId), clientInstanceId.data());

								writeToClient__(CacheResponse, p, cliId, instanceAddress, clientInstanceId);

								++it;
							}
							else 
							{
								it = pCacheItem->clientMap.erase(it);  // erase returns the next valid iterator
							}
						}
					}

					return -1;
				}
			}

			// create new subscription item
			struct CacheItem cacheItem;
			cacheItem.newMsgId = -1;
			cacheItem.lastRequestTime = QDateTime::currentMSecsSinceEpoch();
			cacheItem.lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
			cacheItem.cachedFlag = false;
			cacheItem.methodType = CacheMethodType::SUBSCRIBE_METHOD;
			cacheItem.subscriptionStr = subscriptionStr;
			cacheItem.subscriptionUpdateCount = 0;
			cacheItem.cacheClientId = gWsCacheClientList[cacheClientNumber].clientId;

			// update block and changes
			if (!packetMsg.resultBlock.isEmpty())
			{
				QString msgBlockStr = packetMsg.resultBlock.toLower();
				cacheItem.blockStr = msgBlockStr;
			}

			if (!packetMsg.resultChanges.isEmpty())
			{
				QString msgChangesStr = packetMsg.resultChanges.toLower();

				QStringList changesList = msgChangesStr.split("/");
				for ( const auto& changes : changesList )
				{
					QStringList changeList = changes.split("+");
					if (changeList.size() != 2)
					{
						log_debug("[WS] Invalid change list");
						continue;
					}
					cacheItem.changesMap[changeList[0]] = changeList[1];
				}
			}

			// store response body
			store_cache_response_buffer(subscriptionStr.toUtf8(), responseBuf, QString(""), 0);

			QByteArray subscriptionBytes = subscriptionStr.toUtf8();
			create_cache_item(subscriptionBytes, cacheItem);
			log_debug("[WS] Registered Subscription for \"%s\"", qPrintable(subscriptionStr));

			// make invalild
			return -1;
		}

		if(msgIdValue < 0)
		{
			// make invalild
			log_debug("[WS] detected response without id");
			return -1;
		}

		foreach(QByteArray itemId, get_cache_item_ids())
		{
			CacheItem* pCacheItem = load_cache_item(itemId);
			if (pCacheItem == NULL)
			{
				log_debug("[SCAN] not found cache item %s", itemId.toHex().data());
				continue;
			}
			if ((pCacheItem->proto == Scheme::websocket) && 
				(pCacheItem->newMsgId == msgIdValue) &&
				(pCacheItem->cacheClientId == packetId))
			{
				if (pCacheItem->methodType == CacheMethodType::CACHE_METHOD)
				{
					log_debug("[WS] Adding Cache content for method name=%s", qPrintable(pCacheItem->methodName));

					log_debug("[QQQ] %s, %s, %d", (pCacheItem->cachedFlag==false)?"F":"T", (packetMsg.isResultNull==false)?"F":"T", pCacheItem->retryCount);
					if (pCacheItem->cachedFlag == false && 
						!(pCacheItem->refreshFlag & ACCEPT_NULL_RESPONSE) &&
						packetMsg.isResultNull == true && 
						pCacheItem->retryCount < RETRY_RESPONSE_MAX_COUNT)
					{
						log_debug("[WS] get NULL response, retrying %d", pCacheItem->retryCount);
						pCacheItem->lastAccessTime = QDateTime::currentMSecsSinceEpoch();
						pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();

						int ccIndex = get_cc_index_from_clientId(pCacheItem->cacheClientId);
						// prometheus status
						if (ccIndex >= 0)
						{
							QString urlPath = gWsBackendUrlList[ccIndex];
							if (wsCacheClientInvalidResponseCountMap.contains(urlPath))
								wsCacheClientInvalidResponseCountMap[urlPath]++;
						}

						return 0;
					}
					
					pCacheItem->newMsgId = msgIdValue;
					pCacheItem->cachedFlag = true;

					// store response body
					store_cache_response_buffer(itemId, responseBuf, packetMsg.id, 0);

					// send response to all clients
					QString urlPath = "";
					foreach(QByteArray cliId, pCacheItem->clientMap.keys())
					{
						if (gWsClientMap.contains(cliId))
						{
							QString clientMsgId = pCacheItem->clientMap[cliId].msgId;
							QByteArray clientInstanceId = pCacheItem->clientMap[cliId].instanceId;
							
							if (urlPath.isEmpty())
								urlPath = gWsClientMap[cliId].urlPath;
							log_debug("[WS] Sending Cache content to client id=%s", cliId.data());
							writeToClient_(itemId, cliId, clientMsgId, instanceAddress, clientInstanceId);
						}
					}
					pCacheItem->clientMap.clear();

					// delete cache item once sent response if cache-less one connection is enabled.
					if (pCacheItem->refreshFlag & AUTO_REFRESH_PASSTHROUGH)
					{
						log_debug("[WS] Delete cache item because no auto-refresh");
						remove_cache_item(itemId);
					}
				}
				else if (pCacheItem->methodType == CacheMethodType::SUBSCRIBE_METHOD)
				{
					log_debug("[WS] Adding Subscribe content for method name=%s", qPrintable(pCacheItem->methodName));
					
					// result
					if(msgResultStr.isNull())
					{
						return -1;
					}
					pCacheItem->newMsgId = msgIdValue;
					if ((msgResultStr.compare("true", Qt::CaseInsensitive) != 0) && (msgResultStr.compare("false", Qt::CaseInsensitive) != 0)) 
					{
						pCacheItem->subscriptionStr = msgResultStr;
					}
					else
					{
						return -1;
					}
					
					log_debug("[WS] Registered Subscription result for \"%s\"", qPrintable(msgResultStr));

					// update subscription last update time
					pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();

					// store response body
					store_cache_response_buffer(itemId, responseBuf, packetMsg.id, 0);

					// Search temp teim in SubscriptionItemMap
					QByteArray resultBytes = msgResultStr.toUtf8();
					CacheItem* pResultCacheItem = load_cache_item(resultBytes);
					if (pResultCacheItem != NULL)
					{
						if (pResultCacheItem->newMsgId == -1)
						{
							pCacheItem->cachedFlag = true;
							pCacheItem->subscriptionUpdateCount = 0;
							remove_cache_item(resultBytes);
							log_debug("[WS] Added Subscription content for subscription method id=%d result=%s", msgIdValue, qPrintable(msgResultStr));
						}
					}

					if (pCacheItem->cachedFlag == true)
					{
						QByteArray subscriptionKey = itemId + "-sub";
						// send update subscribe to all clients
						QHash<QByteArray, ClientInCacheItem>::iterator it = pCacheItem->clientMap.begin();
						while (it != pCacheItem->clientMap.end()) 
						{
							QByteArray cliId = it.key();
							if (gWsClientMap.contains(cliId))
							{
								QString clientMsgId = pCacheItem->clientMap[cliId].msgId;
								QByteArray clientInstanceId = pCacheItem->clientMap[cliId].instanceId;

								log_debug("[WS] Sending Subscription content to client id=%s, msgId=%s, instanceId=%s", 
										cliId.data(), qPrintable(clientMsgId), clientInstanceId.data());
								
								writeToClient_(itemId, cliId, clientMsgId, instanceAddress, clientInstanceId);
								writeToClient_(subscriptionKey, cliId, clientMsgId, instanceAddress, clientInstanceId);

								++it;
							}
							else 
							{
								it = pCacheItem->clientMap.erase(it);  // erase returns the next valid iterator
							}
						}
					}
				}
				return -1;
			}
		}

		return 0;
	}

	int send_ws_request_over_cacheclient(const ZhttpRequestPacket &packet, QString orgMsgId, int ccIndex)
	{
		if (ccIndex < 0 || gWsCacheClientList[ccIndex].initFlag == false)
		{
			log_debug("[WS] Invalid cache client %d", ccIndex);
			return -1;
		}

		// Create new packet by cache client
		ZhttpRequestPacket p = packet;
		ClientItem *cacheClient = &gWsCacheClientList[ccIndex];

		ZhttpRequestPacket::Id tempId;
		tempId.id = cacheClient->clientId; // id
		tempId.seq = update_request_seq(cacheClient->clientId);
		p.ids.clear();
		p.ids += tempId;

		int msgId = -1;
		if (!orgMsgId.isEmpty())
		{
			cacheClient->msgIdCount += 1;
			msgId = cacheClient->msgIdCount;
			replace_id_field(p.body, orgMsgId, msgId);
		}

		// log
		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
		{
			QString vrespStr = TnetString::variantToString(p.toVariant(), -1);
			QString logStr;
			if (vrespStr.length() > DEBUG_LOG_MAX_LENGTH)
			{
				logStr = vrespStr.leftRef(DEBUG_LOG_MAX_LENGTH/2) + "........" + vrespStr.rightRef(DEBUG_LOG_MAX_LENGTH/2);
			}
			else
			{
				logStr = vrespStr;
			}
			log_debug("[WS] send_ws_request_over_cacheclient: %s", qPrintable(logStr));
		}

		std::weak_ptr<Private> self = q->d;

		foreach(const ZhttpRequestPacket::Id &id, p.ids)
		{
			// is this for a websocket?
			ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(cacheClient->from, id.id));
			if(sock)
			{
				sock->handle(id.id, id.seq, p);
				if(self.expired())
					return -1;

				continue;
			}
		}
		return msgId;
	}

	int send_unsubscribe_request_over_cacheclient()
	{
		int itemCount = gUnsubscribeRequestMap[instanceId].count();
		if (itemCount > 0)
		{			
			UnsubscribeRequestItem reqItem = gUnsubscribeRequestMap[instanceId][0];
			gUnsubscribeRequestMap[instanceId].removeAt(0);

			// Create new packet by cache client
			ZhttpRequestPacket p;
			ZhttpRequestPacket::Id tempId;

			int ccIndex = get_cc_index_from_clientId(reqItem.cacheClientId);

			if (ccIndex < 0 || gWsCacheClientList[ccIndex].initFlag == false)
			{
				log_debug("[WS] Invalid cache client %d", ccIndex);
				return -1;
			}

			ClientItem *cacheClient = &gWsCacheClientList[ccIndex];

			tempId.id = gWsCacheClientList[ccIndex].clientId; // id
			tempId.seq = update_request_seq(cacheClient->clientId);
			p.ids.append(tempId);

			p.type = ZhttpRequestPacket::Data;
			p.from = reqItem.from;

			char bodyStr[1024];
			gWsCacheClientList[ccIndex].msgIdCount++;
			int msgId = gWsCacheClientList[ccIndex].msgIdCount;
			QString methodName = reqItem.unsubscribeMethodName;
			qsnprintf(bodyStr, 1024, "{\"id\":%d,\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":[\"%s\"]}", 
				msgId, qPrintable(methodName), qPrintable(reqItem.subscriptionStr));
			p.body = QByteArray(bodyStr);

			log_debug("[WS] send_unsubscribeRequest: %s", qPrintable(TnetString::variantToString(p.toVariant(), -1)));

			std::weak_ptr<Private> self = q->d;

			foreach(const ZhttpRequestPacket::Id &id, p.ids)
			{
				// is this for a websocket?
				ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(cacheClient->from, id.id));
				if(sock)
				{
					sock->handle(id.id, id.seq, p);
					if(self.expired())
						return -1;

					continue;
				}
			}
		}

		return 0;
	}

	void delete_old_clients()
	{
		int itemCount = gDeleteClientList.count();
		if (itemCount > 0)
		{
			QByteArray clientId = gDeleteClientList[0];
			gDeleteClientList.removeAt(0);

			unregister_client(clientId);
		}
	}

	int process_ws_stream_request(const QByteArray packetId, ZhttpRequestPacket &p)
	{
		int ret = check_multi_packets_for_ws_request(p);
		if (ret < 0)
			return -1;
		
		// parse json body
		PacketMsg packetMsg;
		if (parse_packet_msg(Scheme::websocket, p, packetMsg, instanceId) < 0)
			return -1;

		// read msgIdStr (id) and methodName (method)
		QString msgIdStr = packetMsg.id;
		QString methodName = packetMsg.method;
		QString msgParams = packetMsg.params;
		if (msgIdStr.isEmpty() || methodName.isEmpty())
		{
			log_debug("[WS] failed to get gMsgIdAttrName and gMsgMethodAttrName");
			return 0;
		}

		// get method string			
		log_debug("[WS] Cache entry msgId=\"%s\" method=\"%s\"", qPrintable(msgIdStr), qPrintable(methodName));

		// update the counter for prometheus
		gCacheMethodRequestCountList.append(methodName);

		// Params hash val
		QByteArray paramsHash = packetMsg.paramsHash;

		if (is_cache_method(methodName) || is_subscribe_method(methodName))
		{
			CacheItem* pCacheItem = load_cache_item(paramsHash, methodName);
			if (pCacheItem != NULL)
			{
				pCacheItem->lastAccessTime = QDateTime::currentMSecsSinceEpoch();

				// prometheus staus
				update_prometheus_hit_count(*pCacheItem);

				if (pCacheItem->cachedFlag == true)
				{
					int ccIndex = get_cc_index_from_clientId(pCacheItem->cacheClientId);
					if (ccIndex < 0 || gWsCacheClientList[ccIndex].initFlag == false)
					{
						ccIndex = get_main_cc_index(instanceId);
						if (ccIndex < 0)
						{
							log_warning("[WS] not initialized cache client, ignore");
							return 0;
						}
					}

					log_debug("[WS] Repling with Cache content for method \"%s\"", qPrintable(methodName));

					if (pCacheItem->methodType == CacheMethodType::CACHE_METHOD)
					{
						writeToClient_(paramsHash, packetId, packetMsg.id, p.from, instanceId);
					}
					else if (pCacheItem->methodType == CacheMethodType::SUBSCRIBE_METHOD)
					{
						QByteArray subscriptionKey = paramsHash + "-sub";
						QByteArray updateCountKey = paramsHash + "-updateCount";
						QByteArray countBytes = redis_load_cache_response(updateCountKey);
						int updateCount = countBytes.toInt();
						pCacheItem->subscriptionUpdateCount = updateCount;
						writeToClient_(paramsHash, packetId, packetMsg.id, p.from, instanceId);
						writeToClient_(subscriptionKey, packetId, packetMsg.id, p.from, instanceId);
						// add client to list
						pCacheItem->clientMap[packetId].msgId = msgIdStr;
						pCacheItem->clientMap[packetId].from = p.from;
						pCacheItem->clientMap[packetId].instanceId = instanceId;
						log_debug("[WS] Adding new client id msgId=%s clientId=%s", qPrintable(msgIdStr), packetId.data());
						pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
					}
				}
				else
				{
					log_debug("[WS] Already cache registered, but not added content \"%s\"", qPrintable(methodName));
					// add client to list
					pCacheItem->clientMap[packetId].msgId = msgIdStr;
					pCacheItem->clientMap[packetId].from = p.from;
					pCacheItem->clientMap[packetId].instanceId = instanceId;
					log_debug("[WS] Adding new client id msgId=%s clientId=%s", qPrintable(msgIdStr), packetId.data());
					pCacheItem->lastRefreshTime = QDateTime::currentMSecsSinceEpoch();
				}
				return -1;
			}
			else
			{
				// Register new cache item
				int ccIndex = register_ws_cache_item(p, packetId, msgIdStr, methodName, msgParams, paramsHash);
				if (ccIndex < 0)
				{
					log_warning("[WS] not initialized cache client, ignore");
					return 0;
				}
				log_debug("[WS] Registered New Cache Item for id=%s method=\"%s\"", qPrintable(msgIdStr), qPrintable(methodName));
				
				pCacheItem = load_cache_item(paramsHash);
				if (pCacheItem == NULL)
				{
					log_debug("[WS_REQ] not found cache item %s", paramsHash.toHex().data());
					return -1;
				}
				// Send new client cache request packet
				pCacheItem->newMsgId = send_ws_request_over_cacheclient(p, msgIdStr, ccIndex);
				pCacheItem->lastRequestTime = QDateTime::currentMSecsSinceEpoch();

				//store_cache_item_field(paramsHash, "lastRequestTime", pCacheItem->lastRequestTime);

				// register cache refresh
				register_cache_refresh(paramsHash, gWsCacheClientList[ccIndex].urlPath);
			}
			
			return -1;
		}

		// log unhitted method
		log_debug("[CACHE ITME] not hit method = %s", qPrintable(methodName));

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

void initCacheClient(int workerNo)
{
	if (gWorkersCount == 1)
	{
		for	(int i=0; i<gWsBackendUrlList.count(); i++)
		{
			log_debug("_[TIMER] init cache client backend=%s", qPrintable(gWsBackendUrlList[i]));

			// add cache client
			ClientItem cacheClient;
			cacheClient.initFlag = false;
			cacheClient.urlPath = gWsBackendUrlList[i];
			cacheClient.lastResponseSeq = -1;
			cacheClient.lastResponseTime = QDateTime::currentMSecsSinceEpoch();
			cacheClient.wscatWorker = nullptr;
			cacheClient.wscatThread = nullptr;
			gWsCacheClientList.append(cacheClient);

			// create processes for cache client
			create_process_for_cacheclient(i);

			QThread::msleep(100);
		}
	}
	else
	{
		log_debug("_[TIMER] init cache client backend=%s", qPrintable(gWsBackendUrlList[workerNo]));

		// add cache client
		ClientItem cacheClient;
		cacheClient.initFlag = false;
		cacheClient.urlPath = gWsBackendUrlList[workerNo];
		cacheClient.lastResponseSeq = -1;
		cacheClient.lastResponseTime = QDateTime::currentMSecsSinceEpoch();
		gWsCacheClientList.append(cacheClient);

		// create processes for cache client
		create_process_for_cacheclient(workerNo);

		workerNo++;
		if (workerNo < gWsBackendUrlList.count())
		{
			QTimer::singleShot(1 * 100, [=]() {
				initCacheClient(workerNo);
			});
		}
	}
	return;
}

void ZhttpManager::setCacheParameters(
	bool cacheEnable,
	const QStringList &httpBackendUrlList,
	const QStringList &wsBackendUrlList,
	const QStringList &cacheMethodList,
	const QStringList &subscribeMethodList,
	const QStringList &neverTimeoutMethodList,
	const QStringList &refreshShorterMethodList,
	const QStringList &refreshLongerMethodList,
	const QStringList &refreshUneraseMethodList,
	const QStringList &refreshExcludeMethodList,
	const QStringList &refreshPassthroughMethodList,
	const QStringList &nullResponseMethodList,
	const QStringList &cacheKeyItemList,
	const QString &msgIdFieldName,
	const QString &msgMethodFieldName,
	const QString &msgParamsFieldName,
	const QStringList &msgErrorFieldList,
	int prometheusRestoreAllowSeconds,
	bool redisEnable,
	const QString &redisHostAddr,
	const int redisPort,
	const int redisPoolCount,
	const QString &redisKeyHeader,
	const QString &replicaMasterAddr,
	const int replicaMasterPort,
	QMap<QString, QStringList> countMethodGroupMap
	)
{
	gWorkersCount++;
	if (gCacheEnable == true)
	{
		log_debug("[CONFIG] already passed");
		return;
	}
	gCacheEnable = cacheEnable;
	gHttpBackendUrlList = httpBackendUrlList;
	gWsBackendUrlList = wsBackendUrlList;

	// method list
	foreach (QString method, cacheMethodList)
	{
		gCacheMethodList.append(method.toLower());
	}
	for (int i = 0; i < subscribeMethodList.count(); i++)
	{
		QStringList tmpList = subscribeMethodList[i].split(u'+');
		if (tmpList.count() == 2)
		{
			gSubscribeMethodMap[tmpList[0].toLower()] = tmpList[1];
		}
	}
	foreach (QString method, neverTimeoutMethodList)
	{
		gNeverTimeoutMethodList.append(method.toLower());
	}
	foreach (QString method, refreshShorterMethodList)
	{
		gRefreshShorterMethodList.append(method.toLower());
	}
	foreach (QString method, refreshLongerMethodList)
	{
		gRefreshLongerMethodList.append(method.toLower());
	}
	foreach (QString method, refreshUneraseMethodList)
	{
		gRefreshUneraseMethodList.append(method.toLower());
	}
	foreach (QString method, refreshExcludeMethodList)
	{
		gRefreshExcludeMethodList.append(method.toLower());
	}
	foreach (QString method, refreshPassthroughMethodList)
	{
		gRefreshPassthroughMethodList.append(method.toLower());
	}
	foreach (QString method, nullResponseMethodList)
	{
		gNullResponseMethodList.append(method.toLower());
	}

	// cache key item list
	for (int i = 0; i < cacheKeyItemList.size(); ++i) 
	{
		int lastDot = cacheKeyItemList[i].lastIndexOf('.');
		if (lastDot != -1) {
			CacheKeyItem keyItem;
			keyItem.keyName = cacheKeyItemList[i].left(lastDot);
			QString flagVal = cacheKeyItemList[i].mid(lastDot + 1);
			if (flagVal == "JSON_VALUE")
				keyItem.flag = ItemFlag::JSON_VALUE;
			else if (flagVal == "JSON_PAIR")
				keyItem.flag = ItemFlag::JSON_PAIR;
			else if (flagVal == "RAW_VALUE")
				keyItem.flag = ItemFlag::RAW_VALUE;
			else
				continue;

			gCacheKeyItemList.append(keyItem);
		} 
		else 
		{
			continue;
		}
	}

	// attributes
	gMsgIdAttrName = msgIdFieldName;
	gMsgMethodAttrName = msgMethodFieldName;
	gMsgParamsAttrName = msgParamsFieldName;
	foreach (QString attr, msgErrorFieldList)
	{
		gErrorAttrList.append(attr.toLower());
	}

	log_debug("[CONFIG] cache %s", gCacheEnable ? "enabled" : "disabled");
	log_debug("[CONFIG] error attributes:");
	for (int i = 0; i < gErrorAttrList.size(); ++i) {
		log_debug("%s", qPrintable(gErrorAttrList[i]));
	}

	log_debug("[CONFIG] gHttpBackendUrlList");
	for (int i = 0; i < gHttpBackendUrlList.size(); ++i) {
		QString connectPath = gHttpBackendUrlList[i];
		log_debug("%s", qPrintable(connectPath));
		httpCacheClientConnectFailedCountMap[connectPath] = 0;
		httpCacheClientInvalidResponseCountMap[connectPath] = 0;
	}

	log_debug("[CONFIG] gWsBackendUrlList");
	for (int i = 0; i < gWsBackendUrlList.size(); ++i) {
		QString connectPath = gWsBackendUrlList[i];
		log_debug("%s", qPrintable(connectPath));
		wsCacheClientConnectFailedCountMap[connectPath] = 0;
		wsCacheClientInvalidResponseCountMap[connectPath] = 0;
	}

	log_debug("[CONFIG] gCacheMethodList");
	for (int i = 0; i < gCacheMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gCacheMethodList[i]));
	}

	log_debug("[CONFIG] gSubscribeMethodMap");
	for (const auto &key : gSubscribeMethodMap.keys()) {
		log_debug("%s:%s", qPrintable(key), qPrintable(gSubscribeMethodMap.value(key)));
	}

	log_debug("[CONFIG] gNeverTimeoutMethodList");
	for (int i = 0; i < gNeverTimeoutMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gNeverTimeoutMethodList[i]));
	}

	log_debug("[CONFIG] gRefreshShorterMethodList");
	for (int i = 0; i < gRefreshShorterMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gRefreshShorterMethodList[i]));
	}

	log_debug("[CONFIG] gRefreshLongerMethodList");
	for (int i = 0; i < gRefreshLongerMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gRefreshLongerMethodList[i]));
	}

	log_debug("[CONFIG] gRefreshUneraseMethodList");
	for (int i = 0; i < gRefreshUneraseMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gRefreshUneraseMethodList[i]));
	}

	log_debug("[CONFIG] gRefreshExcludeMethodList");
	for (int i = 0; i < gRefreshExcludeMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gRefreshExcludeMethodList[i]));
	}

	log_debug("[CONFIG] gRefreshPassthroughMethodList");
	for (int i = 0; i < gRefreshPassthroughMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gRefreshPassthroughMethodList[i]));
	}

	log_debug("[CONFIG] gNullResponseMethodList");
	for (int i = 0; i < gNullResponseMethodList.size(); ++i) {
		log_debug("%s", qPrintable(gNullResponseMethodList[i]));
	}

	log_debug("[CONFIG] gCacheKeyItemList");
	for (int i = 0; i < gCacheKeyItemList.size(); ++i) {
		log_debug("%s, %d", qPrintable(gCacheKeyItemList[i].keyName), gCacheKeyItemList[i].flag);
	}

	log_debug("gMsgIdAttrName = %s", qPrintable(gMsgIdAttrName));
	log_debug("gMsgMethodAttrName = %s", qPrintable(gMsgMethodAttrName));
	log_debug("gMsgParamsAttrName = %s", qPrintable(gMsgParamsAttrName));

	if (gCacheEnable == true)
	{
		if (gWsBackendUrlList.count() == 0)
		{
			log_debug("[WS] not defined ws backend url, exiting");
			exit(0);
		}
		QTimer::singleShot(2 * 1000, [=]() {
			initCacheClient(0);
		});

		QTimer::singleShot(120 * 1000, [=]() {
			check_cache_clients();
		});

		gCacheThread = QtConcurrent::run(cache_thread);
	}

	// prometheus restore allow seconds (default 300)
	gPrometheusRestoreAllowSeconds=prometheusRestoreAllowSeconds;

	// init redis
	gRedisEnable = redisEnable;
	gRedisHostAddr = redisHostAddr;
	gRedisPort = redisPort;
	gRedisPoolCount = redisPoolCount;
	gRedisKeyHeader = redisKeyHeader;
	gReplicaMasterAddr = replicaMasterAddr;
	gReplicaMasterPort = replicaMasterPort;
	log_debug("[CONFIG] redis %s, host=%s, port=%d, pool=%d, keyHeader=%s", gRedisEnable ? "enabled" : "disabled",
		qPrintable(gRedisHostAddr), gRedisPort, gRedisPoolCount, qPrintable(gRedisKeyHeader));
	if (gRedisEnable == true)
	{
		if (gRedisHostAddr == "127.0.0.1")
			redis_removeall_cache_item();

		if (!gReplicaMasterAddr.isEmpty())
		{
			gReplicaFlag = true;
			redis_reset_replica();
		}
	}
	
	// count method group
	log_debug("[CONFIG] count method group");
	foreach(QString groupKey, countMethodGroupMap.keys())
	{
		QString groupTotalStr = groupKey;
		groupMethodCountMap[groupKey] = 0;
		QStringList groupStrList = countMethodGroupMap[groupKey];
		groupTotalStr += ":";
		for (int i = 0; i < groupStrList.count(); i++)
			groupTotalStr += groupStrList[i]+",";
		log_debug("%s", qPrintable(groupTotalStr));
		gCountMethodGroupMap[groupKey] = groupStrList;
	}

	//restore_prometheusStatFromFile();
}

#include "zhttpmanager.moc"
