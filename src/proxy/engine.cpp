/*
 * Copyright (C) 2012-2015 Fanout, Inc.
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

#include "engine.h"

#include <assert.h>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "packet/retryrequestpacket.h"
#include "log.h"
#include "inspectdata.h"
#include "acceptdata.h"
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

#define DEFAULT_HWM 1000

static QByteArray ridToString(const QPair<QByteArray, QByteArray> &rid)
{
	return rid.first + ':' + rid.second;
}

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

	Private(Engine *_q) :
		QObject(_q),
		q(_q),
		destroying(false),
		zhttpIn(0),
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
		sockJsManager(0)
	{
	}

	~Private()
	{
		destroying = true;

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

		domainMap = new DomainMap(config.routesFile);
		connect(domainMap, SIGNAL(changed()), SLOT(domainMap_changed()));

		zhttpIn = new ZhttpManager(this);
		connect(zhttpIn, SIGNAL(requestReady()), SLOT(zhttpIn_requestReady()));
		connect(zhttpIn, SIGNAL(socketReady()), SLOT(zhttpIn_socketReady()));

		zhttpIn->setInstanceId(config.clientId);
		zhttpIn->setServerInSpecs(config.serverInSpecs);
		zhttpIn->setServerInStreamSpecs(config.serverInStreamSpecs);
		zhttpIn->setServerOutSpecs(config.serverOutSpecs);

		zroutes = new ZRoutes(this);
		zroutes->setInstanceId(config.clientId);
		zroutes->setDefaultOutSpecs(config.clientOutSpecs);
		zroutes->setDefaultOutStreamSpecs(config.clientOutStreamSpecs);
		zroutes->setDefaultInSpecs(config.clientInSpecs);

		sockJsManager = new SockJsManager(config.sockJsUrl, this);
		connect(sockJsManager, SIGNAL(sessionReady()), SLOT(sockjs_sessionReady()));

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
			connect(handler_retry_in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(handler_retry_in_readyRead(const QList<QByteArray> &)));
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

		if(!config.statsSpec.isEmpty())
		{
			stats = new StatsManager(this);

			stats->setInstanceId(config.clientId);
			stats->setIpcFileMode(config.ipcFileMode);

			if(!stats->setSpec(config.statsSpec))
			{
				log_error("unable to bind to stats_spec: %s", qPrintable(config.statsSpec));
				return false;
			}
		}

		if(!config.commandSpec.isEmpty())
		{
			command = new ZrpcManager(this);
			command->setBind(true);
			command->setIpcFileMode(config.ipcFileMode);
			connect(command, SIGNAL(requestReady()), SLOT(command_requestReady()));

			if(!command->setServerSpecs(QStringList() << config.commandSpec))
			{
				// zrpcmanager logs error
				return false;
			}
		}

		if(!config.appVersion.isEmpty() && config.updatesCheck)
		{
			new Updater(config.appVersion, zroutes->defaultManager(), this);
		}

		// init zroutes
		domainMap_changed();

		return true;
	}

	void reload()
	{
		domainMap->reload();
	}

	void doProxy(RequestSession *rs, const InspectData *idata = 0, bool isRetry = false)
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

			ps = new ProxySession(zroutes, accept, this);
			connect(ps, SIGNAL(addNotAllowed()), SLOT(ps_addNotAllowed()));
			connect(ps, SIGNAL(finished()), SLOT(ps_finished()));
			connect(ps, SIGNAL(requestSessionDestroyed(RequestSession *, bool)), SLOT(ps_requestSessionDestroyed(RequestSession *, bool)));

			ps->setRoute(route);
			ps->setDefaultSigKey(config.sigIss, config.sigKey);
			ps->setDefaultUpstreamKey(config.upstreamKey);
			ps->setAcceptXForwardedProtocol(config.acceptXForwardedProtocol);
			ps->setUseXForwardedProtocol(config.useXForwardedProtocol);
			ps->setXffRules(config.xffUntrustedRule, config.xffTrustedRule);
			ps->setOrigHeadersNeedMark(config.origHeadersNeedMark);

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
		rs->disconnect(this);

		ps->add(rs);

		if(stats)
		{
			stats->addConnection(ridToString(rs->rid()), route.id, StatsManager::Http, rs->peerAddress(), rs->isHttps(), isRetry);
			stats->addActivity(route.id);
		}
	}

	void doProxySocket(WebSocket *sock, const DomainMap::Entry &route)
	{
		QByteArray cid = connectionManager.addConnection(sock);

		WsProxySession *ps = new WsProxySession(zroutes, &connectionManager, stats, wsControl, this);
		connect(ps, SIGNAL(finishedByPassthrough()), SLOT(wsps_finishedByPassthrough()));

		ps->setDefaultSigKey(config.sigIss, config.sigKey);
		ps->setDefaultUpstreamKey(config.upstreamKey);
		ps->setAcceptXForwardedProtocol(config.acceptXForwardedProtocol);
		ps->setUseXForwardedProtocol(config.useXForwardedProtocol);
		ps->setXffRules(config.xffUntrustedRule, config.xffTrustedRule);
		ps->setOrigHeadersNeedMark(config.origHeadersNeedMark);

		WsProxyItem *i = new WsProxyItem;
		i->ps = ps;
		wsProxyItemsBySession.insert(i->ps, i);

		ps->start(sock, cid, route);

		if(stats)
		{
			stats->addConnection(cid, ps->routeId(), StatsManager::WebSocket, sock->peerAddress(), sock->requestUri().scheme() == "wss", false);
			stats->addActivity(ps->routeId());
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
		QByteArray xfp = headers.get("X-Forwarded-Protocol");
		return (!xfp.isEmpty() && (xfp == "https" || xfp == "wss"));
	}

	void tryTakeRequest()
	{
		if(!canTake())
			return;

		ZhttpRequest *req = zhttpIn->takeNextRequest();
		if(!req)
			return;

		if(config.acceptXForwardedProtocol && isXForwardedProtocolTls(req->requestHeaders()))
			req->setIsTls(true);

		RequestSession *rs = new RequestSession(domainMap, sockJsManager, inspect, inspectChecker, accept, this);
		connect(rs, SIGNAL(inspected(const InspectData &)), SLOT(rs_inspected(const InspectData &)));
		connect(rs, SIGNAL(inspectError()), SLOT(rs_inspectError()));
		connect(rs, SIGNAL(finished()), SLOT(rs_finished()));
		connect(rs, SIGNAL(finishedByAccept()), SLOT(rs_finishedByAccept()));

		rs->setAutoCrossOrigin(config.autoCrossOrigin);
		rs->setPrefetchSize(config.inspectPrefetch);

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

		if(config.acceptXForwardedProtocol && isXForwardedProtocolTls(sock->requestHeaders()))
			sock->setIsTls(true);

		QUrl requestUri = sock->requestUri();

		log_debug("IN ws id=%s, %s", sock->rid().second.data(), requestUri.toEncoded().data());

		bool isSecure = (requestUri.scheme() == "wss");
		QString host = requestUri.host();

		QByteArray encPath = requestUri.path(QUrl::FullyEncoded).toUtf8();

		// look up the route
		DomainMap::Entry route = domainMap->entry(DomainMap::WebSocket, isSecure, host, encPath);

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

	void rs_inspected(const InspectData &idata)
	{
		RequestSession *rs = (RequestSession *)sender();

		// if we get here, then the request must be proxied. if it was to be directly
		//   accepted, then finishedByAccept would have been emitted instead
		assert(idata.doProxy);

		doProxy(rs, &idata);
	}

	void rs_inspectError()
	{
		RequestSession *rs = (RequestSession *)sender();

		// default action is to proxy without sharing
		doProxy(rs);
	}

	void rs_finished()
	{
		RequestSession *rs = (RequestSession *)sender();

		if(stats)
			stats->removeConnection(ridToString(rs->rid()), false);

		requestSessions.remove(rs);
		delete rs;

		tryTakeNext();
	}

	void rs_finishedByAccept()
	{
		RequestSession *rs = (RequestSession *)sender();

		HttpRequestData rd = rs->requestData();
		DomainMap::Entry e = rs->route();

		QString msg = QString("%1 %2").arg(rd.method, rd.uri.toString(QUrl::FullyEncoded));
		QUrl ref = QUrl(QString::fromUtf8(rd.headers.get("Referer")));
		if(!ref.isEmpty())
			msg += QString(" ref=%1").arg(ref.toString(QUrl::FullyEncoded));
		if(!e.id.isEmpty())
			msg += QString(" route=%1").arg(QString::fromUtf8(e.id));
		msg += " int";
		log_info("%s", qPrintable(msg));

		if(stats)
		{
			// add connection so that it becomes lingerable
			stats->addConnection(ridToString(rs->rid()), QByteArray(), StatsManager::Http, rs->peerAddress(), rs->isHttps(), false);
			stats->addActivity(QByteArray());

			// immediately remove since we're accepting
			stats->removeConnection(ridToString(rs->rid()), true);
		}

		requestSessions.remove(rs);
		delete rs;

		tryTakeNext();
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

		if(stats)
			stats->removeConnection(ridToString(rs->rid()), accept);

		tryTakeNext();
	}

	void wsps_finishedByPassthrough()
	{
		WsProxySession *ps = (WsProxySession *)sender();

		WsProxyItem *i = wsProxyItemsBySession.value(ps);
		assert(i);

		if(stats)
			stats->removeConnection(ps->cid(), false);

		wsProxyItemsBySession.remove(i->ps);
		delete i;
		delete ps;

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

		InspectData idata;
		if(p.haveInspectInfo)
		{
			idata.doProxy = p.inspectInfo.doProxy;
			idata.sharingKey = p.inspectInfo.sharingKey;
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

			RequestSession *rs = new RequestSession(domainMap, sockJsManager, inspect, inspectChecker, accept, this);
			requestSessions += rs;

			// note: if the routing table was changed, there's a chance the request
			//   might get a different route id this time around. this could confuse
			//   stats processors tracking route+connection mappings.
			rs->startRetry(zhttpRequest, req.autoCrossOrigin, req.jsonpCallback, req.jsonpExtendedResponse);

			doProxy(rs, p.haveInspectInfo ? &idata : 0, true);
		}
	}

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

bool Engine::start(const Configuration &config)
{
	return d->start(config);
}

void Engine::reload()
{
	d->reload();
}

#include "engine.moc"
