/*
 * Copyright (C) 2012-2023 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
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

#include "engine.h"

#include <assert.h>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "packet/httpresponsedata.h"
#include "packet/retryrequestpacket.h"
#include "packet/statspacket.h"
#include "packet/zrpcrequestpacket.h"
#include "rtimer.h"
#include "log.h"
#include "inspectdata.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"
#include "zwebsocket.h"
#include "websocketoverhttp.h"
#include "domainmap.h"
#include "zroutes.h"
#include "zrpcmanager.h"
#include "zrpcrequest.h"
#include "zrpcchecker.h"
#include "wscontrolmanager.h"
#include "requestsession.h"
#include "proxysession.h"
#include "wsproxysession.h"
#include "statsmanager.h"
#include "connectionmanager.h"
#include "zutil.h"
#include "sockjsmanager.h"
#include "sockjssession.h"
#include "updater.h"
#include "logutil.h"

#define DEFAULT_HWM 1000

class Engine::Private : public QObject
{
	Q_OBJECT

public:
	class ProxyItem
	{
	public:
		bool shared;
		QByteArray key;
		ProxySession *ps;

		ProxyItem() :
			shared(false),
			ps(0)
		{
		}
	};

	class WsProxyItem
	{
	public:
		WsProxySession *ps;

		WsProxyItem() :
			ps(0)
		{
		}
	};

	Engine *q;
	bool destroying;
	Configuration config;
	ZhttpManager *zhttpIn;
	ZhttpManager *intZhttpIn;
	ZRoutes *zroutes;
	ZrpcManager *inspect;
	WsControlManager *wsControl;
	DomainMap *domainMap;
	ZrpcChecker *inspectChecker;
	StatsManager *stats;
	ZrpcManager *command;
	ZrpcManager *accept;
	QZmq::Socket *handler_retry_in_sock;
	QZmq::Valve *handler_retry_in_valve;
	QSet<RequestSession*> requestSessions;
	QHash<QByteArray, ProxyItem*> proxyItemsByKey;
	QHash<ProxySession*, ProxyItem*> proxyItemsBySession;
	QHash<WsProxySession*, WsProxyItem*> wsProxyItemsBySession;
	SockJsManager *sockJsManager;
	ConnectionManager connectionManager;
	Updater *updater;
	LogUtil::Config logConfig;
	Connection changedConnection;
	Connection cmdReqReadyConnection;
	Connection sessionReadyConnection;
	Connection inspectErrorConnection;
	Connection finishedConnection;
	Connection finishedByAcceptConnection;

	Private(Engine *_q) :
		QObject(_q),
		q(_q),
		destroying(false),
		zhttpIn(0),
		intZhttpIn(0),
		zroutes(0),
		inspect(0),
		wsControl(0),
		domainMap(0),
		inspectChecker(0),
		stats(0),
		command(0),
		accept(0),
		handler_retry_in_sock(0),
		handler_retry_in_valve(0),
		sockJsManager(0),
		updater(0)
	{
	}

	~Private()
	{
		destroying = true;

		// need to delete all objects that may have outgoing connections before zroutes

		delete updater;

		QHashIterator<ProxySession*, ProxyItem*> it(proxyItemsBySession);
		while(it.hasNext())
		{
			it.next();
			delete it.key();
			delete it.value();
		}

		proxyItemsBySession.clear();
		proxyItemsByKey.clear();

		QHashIterator<WsProxySession*, WsProxyItem*> wit(wsProxyItemsBySession);
		while(wit.hasNext())
		{
			wit.next();
			delete wit.key();
			delete wit.value();
		}

		wsProxyItemsBySession.clear();

		foreach(RequestSession *rs, requestSessions)
			delete rs;
		requestSessions.clear();

		WebSocketOverHttp::clearDisconnectManager();

		// need to make sure this is deleted before inspect manager
		delete inspectChecker;
		inspectChecker = 0;
	}

	bool start(const Configuration &_config)
	{
		config = _config;

		// up to 10 timers per connection
		RTimer::init(config.connectionsMax * 10);

		logConfig.fromAddress = config.logFrom;
		logConfig.userAgent = config.logUserAgent;

		WebSocketOverHttp::setMaxManagedDisconnects(config.maxWorkers);

		if(!config.routeLines.isEmpty())
		{
			domainMap = new DomainMap(this);
			foreach(const QString &line, config.routeLines)
				domainMap->addRouteLine(line);
		}
		else
			domainMap = new DomainMap(config.routesFile, this);

		changedConnection = domainMap->changed.connect(boost::bind(&Private::domainMap_changed, this));

		zhttpIn = new ZhttpManager(this);
		connect(zhttpIn, &ZhttpManager::requestReady, this, &Private::zhttpIn_requestReady);
		connect(zhttpIn, &ZhttpManager::socketReady, this, &Private::zhttpIn_socketReady);

		zhttpIn->setInstanceId(config.clientId);
		zhttpIn->setServerInSpecs(config.serverInSpecs);
		zhttpIn->setServerInStreamSpecs(config.serverInStreamSpecs);
		zhttpIn->setServerOutSpecs(config.serverOutSpecs);

		if(!config.intServerInSpecs.isEmpty() && !config.intServerInStreamSpecs.isEmpty() && !config.intServerOutSpecs.isEmpty())
		{
			intZhttpIn = new ZhttpManager(this);
			intZhttpIn->setBind(true);
			intZhttpIn->setIpcFileMode(config.ipcFileMode);
			connect(intZhttpIn, &ZhttpManager::requestReady, this, &Private::intZhttpIn_requestReady);

			intZhttpIn->setInstanceId(config.clientId);
			intZhttpIn->setServerInSpecs(config.intServerInSpecs);
			intZhttpIn->setServerInStreamSpecs(config.intServerInStreamSpecs);
			intZhttpIn->setServerOutSpecs(config.intServerOutSpecs);
		}

		zroutes = new ZRoutes(this);
		zroutes->setInstanceId(config.clientId);
		zroutes->setDefaultOutSpecs(config.clientOutSpecs);
		zroutes->setDefaultOutStreamSpecs(config.clientOutStreamSpecs);
		zroutes->setDefaultInSpecs(config.clientInSpecs);

		sockJsManager = new SockJsManager(config.sockJsUrl, this);
		sessionReadyConnection = sockJsManager->sessionReady.connect(boost::bind(&Private::sockjs_sessionReady, this));

		if(!config.inspectSpec.isEmpty())
		{
			inspect = new ZrpcManager(this);
			inspect->setBind(true);
			inspect->setIpcFileMode(config.ipcFileMode);
			if(!inspect->setClientSpecs(QStringList() << config.inspectSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			inspect->setTimeout(config.inspectTimeout);

			inspectChecker = new ZrpcChecker(this);
		}

		if(!config.acceptSpec.isEmpty())
		{
			accept = new ZrpcManager(this);
			accept->setBind(true);
			accept->setIpcFileMode(config.ipcFileMode);
			if(!accept->setClientSpecs(QStringList() << config.acceptSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			// there's no acceptTimeout config option so we'll reuse inspectTimeout
			accept->setTimeout(config.inspectTimeout);
		}

		if(!config.retryInSpec.isEmpty())
		{
			handler_retry_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);

			handler_retry_in_sock->setHwm(DEFAULT_HWM);

			QString errorMessage;
			if(!ZUtil::setupSocket(handler_retry_in_sock, config.retryInSpec, true, config.ipcFileMode, &errorMessage))
			{
				log_error("%s", qPrintable(errorMessage));
				return false;
			}

			handler_retry_in_valve = new QZmq::Valve(handler_retry_in_sock, this);
			connect(handler_retry_in_valve, &QZmq::Valve::readyRead, this, &Private::handler_retry_in_readyRead);
		}

		if(handler_retry_in_valve)
			handler_retry_in_valve->open();

		if(!config.wsControlInSpec.isEmpty() && !config.wsControlOutSpec.isEmpty())
		{
			wsControl = new WsControlManager(this);

			wsControl->setIpcFileMode(config.ipcFileMode);

			if(!wsControl->setInSpec(config.wsControlInSpec))
			{
				log_error("unable to bind to handler_ws_control_in_spec: %s", qPrintable(config.wsControlInSpec));
				return false;
			}

			if(!wsControl->setOutSpec(config.wsControlOutSpec))
			{
				log_error("unable to bind to handler_ws_control_out_spec: %s", qPrintable(config.wsControlOutSpec));
				return false;
			}
		}

		if(!config.statsSpec.isEmpty() || !config.prometheusPort.isEmpty())
		{
			stats = new StatsManager(config.connectionsMax, 0, this);

			connect(stats, &StatsManager::connMax, this, &Private::stats_connMax);

			stats->setInstanceId(config.clientId);
			stats->setIpcFileMode(config.ipcFileMode);
			stats->setConnectionSendEnabled(config.statsConnectionSend);
			stats->setConnectionsMaxSendEnabled(!config.statsConnectionSend);
			stats->setConnectionTtl(config.statsConnectionTtl);
			stats->setConnectionsMaxTtl(config.statsConnectionsMaxTtl);
			stats->setReportInterval(config.statsReportInterval);

			if(!config.statsSpec.isEmpty())
			{
				if(!stats->setSpec(config.statsSpec))
				{
					// statsmanager logs error
					return false;
				}
			}

			if(!config.prometheusPort.isEmpty())
			{
				stats->setPrometheusPrefix(config.prometheusPrefix);

				if(!stats->setPrometheusPort(config.prometheusPort))
				{
					log_error("unable to bind to prometheus port: %s", qPrintable(config.prometheusPort));
					return false;
				}
			}
		}

		if(!config.commandSpec.isEmpty())
		{
			command = new ZrpcManager(this);
			command->setBind(true);
			command->setIpcFileMode(config.ipcFileMode);
			cmdReqReadyConnection = command->requestReady.connect(boost::bind(&Private::command_requestReady, this));

			if(!command->setServerSpecs(QStringList() << config.commandSpec))
			{
				// zrpcmanager logs error
				return false;
			}
		}

		if(!config.appVersion.isEmpty() && (config.updatesCheck == "check" || config.updatesCheck == "report"))
		{
			updater = new Updater(config.updatesCheck == "report" ? Updater::ReportMode : Updater::CheckMode, config.quietCheck, config.appVersion, config.organizationName, zroutes->defaultManager(), this);
		}

		// init zroutes
		domainMap_changed();

		return true;
	}

	void reload()
	{
		domainMap->reload();
	}

	void doProxy(RequestSession *rs, const InspectData *idata = 0)
	{
		DomainMap::Entry route = rs->route();

		// we'll always have a route
		assert(!route.isNull());

		bool sharable = (idata && !idata->sharingKey.isEmpty() && rs->haveCompleteRequestBody());

		ProxySession *ps = 0;
		if(sharable)
		{
			log_debug("need to proxy with sharing key: %s", idata->sharingKey.data());

			ProxyItem *i = proxyItemsByKey.value(idata->sharingKey);
			if(i)
				ps = i->ps;
		}

		if(!ps)
		{
			log_debug("creating proxysession for id=%s", rs->rid().second.data());

			ps = new ProxySession(zroutes, accept, logConfig, stats);
			// TODO: use callbacks for performance
			connect(ps, &ProxySession::addNotAllowed, this, &Private::ps_addNotAllowed);
			connect(ps, &ProxySession::finished, this, &Private::ps_finished);
			connect(ps, &ProxySession::requestSessionDestroyed, this, &Private::ps_requestSessionDestroyed);

			ps->setRoute(route);
			ps->setDefaultSigKey(config.sigIss, config.sigKey);
			ps->setAcceptXForwardedProtocol(config.acceptXForwardedProto);
			ps->setUseXForwardedProtocol(config.setXForwardedProto, config.setXForwardedProtocol);
			ps->setXffRules(config.xffUntrustedRule, config.xffTrustedRule);
			ps->setOrigHeadersNeedMark(config.origHeadersNeedMark);
			ps->setAcceptPushpinRoute(config.acceptPushpinRoute);
			ps->setCdnLoop(config.cdnLoop);
			ps->setProxyInitialResponseEnabled(true);

			if(idata)
				ps->setInspectData(*idata);

			ProxyItem *i = new ProxyItem;
			i->ps = ps;
			proxyItemsBySession.insert(i->ps, i);

			if(sharable)
			{
				i->shared = true;
				i->key = idata->sharingKey;
				proxyItemsByKey.insert(i->key, i);
			}
		}
		else
			log_debug("reusing proxysession");

		// proxysession will take it from here
		// TODO: use callbacks for performance
		rs->disconnect(this);

		ps->add(rs);
	}

	void doProxySocket(WebSocket *sock, const DomainMap::Entry &route)
	{
		QByteArray cid = connectionManager.addConnection(sock);

		WsProxySession *ps = new WsProxySession(zroutes, &connectionManager, logConfig, stats, wsControl);
		ps->finishedByPassthroughCallback().add(Private::wsps_finishedByPassthrough_cb, this);

		connectionManager.setProxyForConnection(sock, ps);

		ps->setDebugEnabled(config.debug || route.debug);
		ps->setDefaultSigKey(config.sigIss, config.sigKey);
		ps->setDefaultUpstreamKey(config.upstreamKey);
		ps->setAcceptXForwardedProtocol(config.acceptXForwardedProto);
		ps->setUseXForwardedProtocol(config.setXForwardedProto, config.setXForwardedProtocol);
		ps->setXffRules(config.xffUntrustedRule, config.xffTrustedRule);
		ps->setOrigHeadersNeedMark(config.origHeadersNeedMark);
		ps->setAcceptPushpinRoute(config.acceptPushpinRoute);
		ps->setCdnLoop(config.cdnLoop);

		WsProxyItem *i = new WsProxyItem;
		i->ps = ps;
		wsProxyItemsBySession.insert(i->ps, i);

		// after this call, ps->logicalClientAddress() will be valid
		ps->start(sock, cid, route);

		if(stats)
		{
			stats->addConnection(cid, ps->statsRoute(), StatsManager::WebSocket, ps->logicalClientAddress(), sock->requestUri().scheme() == "wss", false);
			stats->addActivity(ps->statsRoute());
			stats->addRequestsReceived(1);
		}
	}

	bool canTake()
	{
		// don't accept new connections during shutdown
		if(destroying)
			return false;

		// don't accept new connections if we're servicing maximum
		int curWorkers = requestSessions.count() + wsProxyItemsBySession.count();
		if(config.maxWorkers != -1 && curWorkers >= config.maxWorkers)
			return false;

		return true;
	}

	bool isXForwardedProtocolTls(const HttpHeaders &headers)
	{
		QByteArray xfp = headers.get("X-Forwarded-Proto");
		if(xfp.isEmpty())
			xfp = headers.get("X-Forwarded-Protocol");
		return (!xfp.isEmpty() && (xfp == "https" || xfp == "wss"));
	}

	void tryTakeRequest()
	{
		if(!canTake())
			return;

		// prioritize external requests over internal requests

		ZhttpRequest *req = zhttpIn->takeNextRequest();
		if(!req)
		{
			if(intZhttpIn)
				req = intZhttpIn->takeNextRequest();

			if(!req)
				return;
		}

		QString routeId;
		bool preferInternal = false;
		bool autoShare = false;

		QVariant passthroughData = req->passthroughData();
		if(passthroughData.isValid())
		{
			// passthrough request, from handler

			const QVariantHash data = passthroughData.toHash();

			// there is always a route
			routeId = QString::fromUtf8(data["route"].toByteArray());

			if(data.contains("prefer-internal"))
				preferInternal = data["prefer-internal"].toBool();

			if(data.contains("auto-share"))
				autoShare = data["auto-share"].toBool();
		}
		else
		{
			// regular request

			if(config.acceptXForwardedProto && isXForwardedProtocolTls(req->requestHeaders()))
				req->setIsTls(true);

			if(config.acceptPushpinRoute)
				routeId = QString::fromUtf8(req->requestHeaders().get("Pushpin-Route"));
		}

		RequestSession *rs;
		if(passthroughData.isValid() && !preferInternal)
		{
			// passthrough request with preferInternal=false. in this case,
			// set up a direct route, using some settings from the original
			// route

			DomainMap::Entry originalRoute;
			if(!routeId.isEmpty() && !domainMap->isIdShared(routeId))
				originalRoute = domainMap->entry(routeId);

			const QVariantHash data = passthroughData.toHash();

			DomainMap::Entry route;

			// use sig settings from the original route, if available
			if(!originalRoute.isNull())
			{
				route.sigIss = originalRoute.sigIss;
				route.sigKey = originalRoute.sigKey;
			}

			DomainMap::Target target;
			QUrl uri = req->requestUri();
			bool isHttps = (uri.scheme() == "https");
			target.connectHost = uri.host();
			target.connectPort = uri.port(isHttps ? 443 : 80);
			target.ssl = isHttps;
			target.trusted = data["trusted"].toBool();

			route.targets += target;

			rs = new RequestSession(stats);
			rs->setRoute(route);
		}
		else
		{
			// regular request (with or without a route ID), or a passthrough
			// request with preferInternal=true. in that case, use domainmap
			// for lookup, with route ID if available

			rs = new RequestSession(domainMap, sockJsManager, inspect, inspectChecker, accept, stats);
			rs->setDebugEnabled(config.debug);
			rs->setAutoCrossOrigin(config.autoCrossOrigin);
			rs->setPrefetchSize(config.inspectPrefetch);
			rs->setDefaultUpstreamKey(config.upstreamKey);
			rs->setXffRules(config.xffUntrustedRule, config.xffTrustedRule);
			rs->setRouteId(routeId);
		}

		rs->setAutoShare(autoShare);

		// TODO: use callbacks for performance
		connect(rs, &RequestSession::inspected, this, &Private::rs_inspected);
		inspectErrorConnection = rs->inspectError.connect(boost::bind(&Private::rs_inspectError, this, rs));
		finishedConnection = rs->finished.connect(boost::bind(&Private::rs_finished, this, rs));
		finishedByAcceptConnection = rs->finishedByAccept.connect(boost::bind(&Private::rs_finishedByAccept, this, rs));

		requestSessions += rs;

		rs->start(req);
	}

	void tryTakeSocket()
	{
		if(!canTake())
			return;

		ZWebSocket *sock = zhttpIn->takeNextSocket();
		if(!sock)
			return;

		if(config.acceptXForwardedProto && isXForwardedProtocolTls(sock->requestHeaders()))
			sock->setIsTls(true);

		QUrl requestUri = sock->requestUri();

		log_debug("IN ws id=%s, %s", sock->rid().second.data(), requestUri.toEncoded().data());

		bool isSecure = (requestUri.scheme() == "wss");
		QString host = requestUri.host();

		QByteArray encPath = requestUri.path(QUrl::FullyEncoded).toUtf8();

		QString routeId;

		if(config.acceptPushpinRoute)
			routeId = QString::fromUtf8(sock->requestHeaders().get("Pushpin-Route"));

		// look up the route
		DomainMap::Entry route;
		if(!routeId.isEmpty() && !domainMap->isIdShared(routeId))
			route = domainMap->entry(routeId);
		else
			route = domainMap->entry(DomainMap::WebSocket, isSecure, host, encPath);

		// before we do anything else, see if this is a sockjs request
		if(!route.isNull() && !route.sockJsPath.isEmpty() && encPath.startsWith(route.sockJsPath))
		{
			sockJsManager->giveSocket(sock, route.sockJsPath.length(), route.sockJsAsPath, route);
			return;
		}

		log_debug("creating wsproxysession for zws id=%s", sock->rid().second.data());
		doProxySocket(sock, route);
	}

	void tryTakeSockJsSession()
	{
		if(!canTake())
			return;

		SockJsSession *sock = sockJsManager->takeNext();
		if(!sock)
			return;

		log_debug("IN sockjs obj=%p %s", sock, sock->requestUri().toEncoded().data());

		log_debug("creating wsproxysession for sockjs=%p", sock);
		doProxySocket(sock, sock->route());
	}

	void tryTakeNext()
	{
		tryTakeRequest();
		tryTakeSocket();
		tryTakeSockJsSession();
	}

	void logFinished(RequestSession *rs, bool accepted = false)
	{
		HttpResponseData resp = rs->responseData();

		LogUtil::RequestData rd;

		DomainMap::Entry route = rs->route();

		// only log route id if explicitly set
		if(route.separateStats)
			rd.routeId = route.id;

		if(accepted)
		{
			rd.status = LogUtil::Accept;
		}
		else if(resp.code != -1)
		{
			rd.status = LogUtil::Response;
			rd.responseData = resp;
			rd.responseBodySize = rs->responseBodySize();
		}
		else
		{
			rd.status = LogUtil::Error;
		}

		rd.requestData = rs->requestData();

		rd.fromAddress = rs->logicalPeerAddress();

		LogUtil::logRequest(LOG_LEVEL_INFO, rd, logConfig);
	}

private:
	void rs_inspectError(RequestSession *rs)
	{
		// default action is to proxy without sharing
		doProxy(rs);
	}

	void rs_finished(RequestSession *rs)
	{
		if(!rs->isSockJs())
			logFinished(rs);

		requestSessions.remove(rs);
		delete rs;

		tryTakeNext();
	}

	void rs_finishedByAccept(RequestSession *rs)
	{
		logFinished(rs, true);

		requestSessions.remove(rs);
		delete rs;

		tryTakeNext();
	}

private slots:
	void zhttpIn_requestReady()
	{
		tryTakeNext();
	}

	void zhttpIn_socketReady()
	{
		tryTakeNext();
	}

	void sockjs_sessionReady()
	{
		tryTakeNext();
	}

	void intZhttpIn_requestReady()
	{
		tryTakeNext();
	}

	void rs_inspected(const InspectData &idata)
	{
		RequestSession *rs = (RequestSession *)sender();

		// if we get here, then the request must be proxied. if it was to be directly
		//   accepted, then finishedByAccept would have been emitted instead
		assert(idata.doProxy);

		doProxy(rs, &idata);
	}

	void ps_addNotAllowed()
	{
		ProxySession *ps = (ProxySession *)sender();

		ProxyItem *i = proxyItemsBySession.value(ps);
		assert(i);

		// no more sharing for this session
		if(i->shared)
		{
			i->shared = false;
			proxyItemsByKey.remove(i->key);
		}
	}

	void ps_finished()
	{
		ProxySession *ps = (ProxySession *)sender();

		ProxyItem *i = proxyItemsBySession.value(ps);
		assert(i);

		if(i->shared)
			proxyItemsByKey.remove(i->key);
		proxyItemsBySession.remove(i->ps);
		delete i;
		delete ps;

		tryTakeNext();
	}

	void ps_requestSessionDestroyed(RequestSession *rs, bool accept)
	{
		requestSessions.remove(rs);

		rs->setAccepted(accept);

		tryTakeNext();
	}

	static void wsps_finishedByPassthrough_cb(void *data, std::tuple<WsProxySession *> value)
	{
		Q_UNUSED(value);

		Private *self = (Private *)data;

		self->wsps_finishedByPassthrough(std::get<0>(value));
	}

	void wsps_finishedByPassthrough(WsProxySession *ps)
	{
		WsProxyItem *i = wsProxyItemsBySession.value(ps);
		assert(i);

		if(stats)
			stats->removeConnection(ps->cid(), false);

		wsProxyItemsBySession.remove(i->ps);
		delete i;

		ps->finishedByPassthroughCallback().remove(this);
		ps->deleteLater();

		tryTakeNext();
	}

	void handler_retry_in_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("retry: received message with parts != 1, skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(message[0], 0, &ok);
		if(!ok)
		{
			log_warning("retry: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("retry: IN %s", qPrintable(TnetString::variantToString(data, -1)));

		RetryRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("retry: received message with invalid format (parse failed), skipping");
			return;
		}

		log_debug("IN (retry) %s %s", qPrintable(p.requestData.method), p.requestData.uri.toEncoded().data());

		if(p.retrySeq >= 0)
			stats->setRetrySeq(p.route, p.retrySeq);

		InspectData idata;
		if(p.haveInspectInfo)
		{
			idata.doProxy = p.inspectInfo.doProxy;
			idata.sharingKey = p.inspectInfo.sharingKey;
			idata.sid = p.inspectInfo.sid;
			idata.lastIds = p.inspectInfo.lastIds;
			idata.userData = p.inspectInfo.userData;
		}

		foreach(const RetryRequestPacket::Request &req, p.requests)
		{
			ZhttpRequest::ServerState ss;
			ss.rid = ZhttpRequest::Rid(req.rid.first, req.rid.second);
			ss.peerAddress = req.peerAddress;
			ss.requestMethod = p.requestData.method;
			ss.requestUri = p.requestData.uri;
			if(req.https)
				ss.requestUri.setScheme("https");
			ss.requestHeaders = p.requestData.headers;
			ss.requestBody = p.requestData.body;
			ss.inSeq = req.inSeq;
			ss.outSeq = req.outSeq;
			ss.outCredits = req.outCredits;
			ss.userData = req.userData;

			ZhttpRequest *zhttpRequest = zhttpIn->createRequestFromState(ss);

			RequestSession *rs = new RequestSession(domainMap, sockJsManager, inspect, inspectChecker, accept, stats);

			requestSessions += rs;

			rs->setDefaultUpstreamKey(config.upstreamKey);
			rs->setXffRules(config.xffUntrustedRule, config.xffTrustedRule);

			if(!p.route.isEmpty())
				rs->setRouteId(QString::fromUtf8(p.route));

			// note: if the routing table was changed, there's a chance the request
			//   might get a different route id this time around. this could confuse
			//   stats processors tracking route+connection mappings.
			rs->startRetry(zhttpRequest, req.debug, req.autoCrossOrigin, req.jsonpCallback, req.jsonpExtendedResponse, req.unreportedTime);

			doProxy(rs, p.haveInspectInfo ? &idata : 0);
		}
	}

	void stats_connMax(const StatsPacket &packet)
	{
		if(accept->canWriteImmediately())
		{
			ZrpcRequestPacket p;
			p.method = "conn-max";
			p.args["conn-max"] = QVariantList() << packet.toVariant();

			accept->write(p);
		}
	}

private:
	void command_requestReady()
	{
		ZrpcRequest *req = command->takeNext();
		if(req->method() == "conncheck")
		{
			if(!stats)
			{
				req->respondError("service-unavailable");
				delete req;
				return;
			}

			QVariantHash args = req->args();
			if(!args.contains("ids") || args["ids"].type() != QVariant::List)
			{
				req->respondError("bad-format");
				delete req;
				return;
			}

			QVariantList vids = args["ids"].toList();

			bool ok = true;
			QList<QByteArray> ids;
			foreach(const QVariant &vid, vids)
			{
				if(vid.type() != QVariant::ByteArray)
				{
					ok = false;
					break;
				}

				ids += vid.toByteArray();
			}
			if(!ok)
			{
				req->respondError("bad-format");
				delete req;
				return;
			}

			QVariantList out;
			foreach(const QByteArray &id, ids)
			{
				if(stats->checkConnection(id))
					out += id;
			}

			req->respond(out);
		}
		else if(req->method() == "refresh")
		{
			QVariantHash args = req->args();
			if(!args.contains("cid") || args["cid"].type() != QVariant::ByteArray)
			{
				req->respondError("bad-format");
				delete req;
				return;
			}

			QByteArray cid = args["cid"].toByteArray();

			WsProxySession *ps = connectionManager.getProxyForConnection(cid);
			if(!ps)
			{
				req->respondError("item-not-found");
				delete req;
				return;
			}

			WebSocketOverHttp *woh = qobject_cast<WebSocketOverHttp*>(ps->outSocket());
			if(woh)
				woh->refresh();

			req->respond();
		}
		else if(req->method() == "report")
		{
			QVariantHash args = req->args();
			if(!args.contains("stats") || args["stats"].type() != QVariant::Hash)
			{
				req->respondError("bad-format");
				delete req;
				return;
			}

			QVariant data = args["stats"];

			StatsPacket p;
			if(!p.fromVariant("report", data))
			{
				req->respondError("bad-format");
				delete req;
				return;
			}

			if(!updater)
			{
				req->respondError("service-unavailable");
				delete req;
				return;
			}

			int connectionsMax = qMax(p.connectionsMax, 0);
			int connectionsMinutes = qMax(p.connectionsMinutes, 0);
			int messagesReceived = qMax(p.messagesReceived, 0);
			int messagesSent = qMax(p.messagesSent, 0);
			int httpResponseMessagesSent = qMax(p.httpResponseMessagesSent, 0);

			Updater::Report report;
			report.connectionsMax = connectionsMax;
			report.connectionsMinutes = connectionsMinutes;
			report.messagesReceived = messagesReceived;
			report.messagesSent = messagesSent;

			// fanout cloud style ops calculation
			report.ops = connectionsMinutes + messagesReceived + messagesSent - httpResponseMessagesSent;

			updater->setReport(report);

			req->respond();
		}
		else
		{
			req->respondError("method-not-found");
		}

		delete req;
	}

	void domainMap_changed()
	{
		// connect to new zhttp targets, disconnect from old
		zroutes->setup(domainMap->zhttpRoutes());
	}
};

Engine::Engine(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

Engine::~Engine()
{
	delete d;
}

StatsManager *Engine::statsManager() const
{
	return d->stats;
}

bool Engine::start(const Configuration &config)
{
	return d->start(config);
}

void Engine::reload()
{
	d->reload();
}

#include "engine.moc"
