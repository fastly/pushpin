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
#include "packet/acceptresponsepacket.h"
#include "packet/retryrequestpacket.h"
#include "log.h"
#include "inspectdata.h"
#include "acceptdata.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"
#include "zwebsocket.h"
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
	QZmq::Socket *handler_retry_in_sock;
	QZmq::Socket *handler_accept_out_sock;
	QZmq::Valve *handler_retry_in_valve;
	QSet<RequestSession*> requestSessions;
	QHash<QByteArray, ProxyItem*> proxyItemsByKey;
	QHash<ProxySession*, ProxyItem*> proxyItemsBySession;
	QHash<WsProxySession*, WsProxyItem*> wsProxyItemsBySession;

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
		handler_retry_in_sock(0),
		handler_accept_out_sock(0),
		handler_retry_in_valve(0)
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

		if(!config.inspectSpec.isEmpty())
		{
			inspect = new ZrpcManager(this);
			inspect->setBind(true);
			if(!inspect->setClientSpecs(QStringList() << config.inspectSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			inspect->setTimeout(config.inspectTimeout);

			inspectChecker = new ZrpcChecker(this);
		}

		if(!config.retryInSpec.isEmpty())
		{
			handler_retry_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);

			handler_retry_in_sock->setHwm(DEFAULT_HWM);

			if(!handler_retry_in_sock->bind(config.retryInSpec))
			{
				log_error("unable to bind to handler_retry_in_spec: %s", qPrintable(config.retryInSpec));
				return false;
			}

			handler_retry_in_valve = new QZmq::Valve(handler_retry_in_sock, this);
			connect(handler_retry_in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(handler_retry_in_readyRead(const QList<QByteArray> &)));
		}

		if(!config.acceptOutSpec.isEmpty())
		{
			handler_accept_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);

			handler_accept_out_sock->setHwm(DEFAULT_HWM);

			connect(handler_accept_out_sock, SIGNAL(messagesWritten(int)), SLOT(handler_accept_out_messagesWritten(int)));
			if(!handler_accept_out_sock->bind(config.acceptOutSpec))
			{
				log_error("unable to bind to handler_accept_out_spec: %s", qPrintable(config.acceptOutSpec));
				return false;
			}
		}

		if(handler_retry_in_valve)
			handler_retry_in_valve->open();

		if(!config.wsControlInSpec.isEmpty() && !config.wsControlOutSpec.isEmpty())
		{
			wsControl = new WsControlManager(this);

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
			connect(command, SIGNAL(requestReady()), SLOT(command_requestReady()));

			if(!command->setServerSpecs(QStringList() << config.commandSpec))
			{
				// zrpcmanager logs error
				return false;
			}
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

			ps = new ProxySession(zroutes, this);
			connect(ps, SIGNAL(addNotAllowed()), SLOT(ps_addNotAllowed()));
			connect(ps, SIGNAL(finishedByPassthrough()), SLOT(ps_finishedByPassthrough()));
			connect(ps, SIGNAL(finishedForAccept(const AcceptData &)), SLOT(ps_finishedForAccept(const AcceptData &)));
			connect(ps, SIGNAL(requestSessionDestroyed(RequestSession *, bool)), SLOT(ps_requestSessionDestroyed(RequestSession *, bool)));

			ps->setRoute(route);
			ps->setDefaultSigKey(config.sigIss, config.sigKey);
			ps->setDefaultUpstreamKey(config.upstreamKey);
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

	void sendAccept(const AcceptData &adata)
	{
		AcceptResponsePacket p;
		foreach(const AcceptData::Request &areq, adata.requests)
		{
			AcceptResponsePacket::Request req;
			req.rid = AcceptResponsePacket::Rid(areq.rid.first, areq.rid.second);
			req.https = areq.https;
			req.peerAddress = areq.peerAddress;
			req.autoCrossOrigin = areq.autoCrossOrigin;
			req.jsonpCallback = areq.jsonpCallback;
			req.jsonpExtendedResponse = areq.jsonpExtendedResponse;
			req.inSeq = areq.inSeq;
			req.outSeq = areq.outSeq;
			req.outCredits = areq.outCredits;
			req.userData = areq.userData;
			p.requests += req;
		}

		p.requestData = adata.requestData;

		if(adata.haveInspectData)
		{
			p.haveInspectInfo = true;
			p.inspectInfo.noProxy = !adata.inspectData.doProxy;
			p.inspectInfo.sharingKey = adata.inspectData.sharingKey;
			p.inspectInfo.userData = adata.inspectData.userData;
		}

		if(adata.haveResponse)
		{
			p.haveResponse = true;
			p.response = adata.response;
		}

		p.route = adata.route;
		p.channelPrefix = adata.channelPrefix;

		QList<QByteArray> msg;
		msg += TnetString::fromVariant(p.toVariant());
		handler_accept_out_sock->write(msg);
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

	void tryTakeRequest()
	{
		if(!canTake())
			return;

		ZhttpRequest *req = zhttpIn->takeNextRequest();
		if(!req)
			return;

		RequestSession *rs = new RequestSession(domainMap, inspect, inspectChecker, this);
		connect(rs, SIGNAL(inspected(const InspectData &)), SLOT(rs_inspected(const InspectData &)));
		connect(rs, SIGNAL(inspectError()), SLOT(rs_inspectError()));
		connect(rs, SIGNAL(finished()), SLOT(rs_finished()));
		connect(rs, SIGNAL(finishedForAccept(const AcceptData &)), SLOT(rs_finishedForAccept(const AcceptData &)));

		rs->setAutoCrossOrigin(config.autoCrossOrigin);

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

		log_debug("creating wsproxysession for id=%s", sock->rid().second.data());

		WsProxySession *ps = new WsProxySession(zroutes, domainMap, stats, wsControl, this);
		connect(ps, SIGNAL(finishedByPassthrough()), SLOT(wsps_finishedByPassthrough()));

		ps->setDefaultSigKey(config.sigIss, config.sigKey);
		ps->setDefaultUpstreamKey(config.upstreamKey);
		ps->setUseXForwardedProtocol(config.useXForwardedProtocol);
		ps->setXffRules(config.xffUntrustedRule, config.xffTrustedRule);
		ps->setOrigHeadersNeedMark(config.origHeadersNeedMark);

		WsProxyItem *i = new WsProxyItem;
		i->ps = ps;
		wsProxyItemsBySession.insert(i->ps, i);

		ps->start(sock);

		if(stats)
		{
			stats->addConnection(ridToString(sock->rid()), ps->routeId(), StatsManager::WebSocket, sock->peerAddress(), sock->requestUri().scheme() == "wss", false);
			stats->addActivity(ps->routeId());
		}
	}

	void tryTakeNext()
	{
		tryTakeRequest();
		tryTakeSocket();
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

	void rs_inspected(const InspectData &idata)
	{
		RequestSession *rs = (RequestSession *)sender();

		// if we get here, then the request must be proxied. if it was to be directly
		//   accepted, then finishedForAccept would have been emitted instead
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

	void rs_finishedForAccept(const AcceptData &adata)
	{
		RequestSession *rs = (RequestSession *)sender();

		if(!handler_accept_out_sock->canWriteImmediately())
		{
			rs->respondCannotAccept();
			return;
		}

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

		sendAccept(adata);

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

	void ps_finishedByPassthrough()
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

	void ps_finishedForAccept(const AcceptData &adata)
	{
		ProxySession *ps = (ProxySession *)sender();

		if(!handler_accept_out_sock->canWriteImmediately())
		{
			ps->cannotAccept();
			return;
		}

		ProxyItem *i = proxyItemsBySession.value(ps);
		assert(i);

		if(i->shared)
			proxyItemsByKey.remove(i->key);
		proxyItemsBySession.remove(i->ps);
		delete i;

		// accept from ProxySession always has a response
		assert(adata.haveResponse);

		sendAccept(adata);

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
			stats->removeConnection(ridToString(ps->rid()), false);

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

		RetryRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("retry: received message with invalid format (parse failed), skipping");
			return;
		}

		log_info("retry: IN %s %s", qPrintable(p.requestData.method), p.requestData.uri.toEncoded().data());

		InspectData idata;
		if(p.haveInspectInfo)
		{
			idata.doProxy = !p.inspectInfo.noProxy;
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
			ss.inSeq = req.inSeq;
			ss.outSeq = req.outSeq;
			ss.outCredits = req.outCredits;
			ss.userData = req.userData;

			ZhttpRequest *zhttpRequest = zhttpIn->createRequestFromState(ss);

			RequestSession *rs = new RequestSession(domainMap, inspect, inspectChecker, this);
			rs->startRetry(zhttpRequest, req.autoCrossOrigin, req.jsonpCallback, req.jsonpExtendedResponse);

			requestSessions += rs;

			// note: if the routing table was changed, there's a chance the request
			//   might get a different route id this time around. this could confuse
			//   stats processors tracking route+connection mappings.

			doProxy(rs, p.haveInspectInfo ? &idata : 0, true);
		}
	}

	void handler_accept_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
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
