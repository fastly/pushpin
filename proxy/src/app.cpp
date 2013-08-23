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

#include "app.h"

#include <assert.h>
#include <QCoreApplication>
#include <QStringList>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "packet/acceptresponsepacket.h"
#include "packet/retryrequestpacket.h"
#include "processquit.h"
#include "log.h"
#include "inspectdata.h"
#include "acceptdata.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"
#include "domainmap.h"
#include "xffrule.h"
#include "inspectmanager.h"
#include "inspectchecker.h"
#include "requestsession.h"
#include "proxysession.h"

#define VERSION "1.0"

#define DEFAULT_HWM 1000

static void trimlist(QStringList *list)
{
	for(int n = 0; n < list->count(); ++n)
	{
		if((*list)[n].isEmpty())
		{
			list->removeAt(n);
			--n; // adjust position
		}
	}
}

static QByteArray parse_key(const QString &in)
{
	if(in.startsWith("base64:"))
		return QByteArray::fromBase64(in.mid(7).toUtf8());
	else
		return in.toUtf8();
}

static XffRule parse_xffRule(const QStringList &in)
{
	XffRule out;
	foreach(const QString &s, in)
	{
		if(s.startsWith("truncate:"))
		{
			bool ok;
			int x = s.mid(9).toInt(&ok);
			if(!ok)
				return out;

			out.truncate = x;
		}
		else if(s == "append")
			out.append = true;
	}
	return out;
}

class App::Private : public QObject
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

	App *q;
	bool verbose;
	QByteArray clientId;
	ZhttpManager *zhttp;
	InspectManager *inspect;
	DomainMap *domainMap;
	InspectChecker *inspectChecker;
	QZmq::Socket *handler_retry_in_sock;
	QZmq::Socket *handler_accept_out_sock;
	QZmq::Valve *handler_retry_in_valve;
	QSet<RequestSession*> requestSessions;
	QHash<QByteArray, ProxyItem*> proxyItemsByKey;
	QHash<ProxySession*, ProxyItem*> proxyItemsBySession;
	int maxWorkers;
	bool autoCrossOrigin;
	bool useXForwardedProtocol;
	XffRule xffRule;
	XffRule xffTrustedRule;
	QList<QByteArray> origHeadersNeedMark;
	QByteArray sigIss;
	QByteArray sigKey;
	QByteArray upstreamKey;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		verbose(false),
		zhttp(0),
		inspect(0),
		domainMap(0),
		inspectChecker(0),
		handler_retry_in_sock(0),
		handler_accept_out_sock(0),
		handler_retry_in_valve(0)
	{
		connect(ProcessQuit::instance(), SIGNAL(quit()), SLOT(doQuit()));
		connect(ProcessQuit::instance(), SIGNAL(hup()), SLOT(reload()));
	}

	~Private()
	{
		QHashIterator<ProxySession*, ProxyItem*> it(proxyItemsBySession);
		while(it.hasNext())
		{
			it.next();
			delete it.key();
			delete it.value();
		}

		proxyItemsBySession.clear();
		proxyItemsByKey.clear();
		requestSessions.clear();
	}

	void start()
	{
		QStringList args = QCoreApplication::instance()->arguments();
		args.removeFirst();

		// options
		QHash<QString, QString> options;
		for(int n = 0; n < args.count(); ++n)
		{
			if(args[n] == "--")
			{
				break;
			}
			else if(args[n].startsWith("--"))
			{
				QString opt = args[n].mid(2);
				QString var, val;

				int at = opt.indexOf("=");
				if(at != -1)
				{
					var = opt.mid(0, at);
					val = opt.mid(at + 1);
				}
				else
					var = opt;

				options[var] = val;

				args.removeAt(n);
				--n; // adjust position
			}
		}

		if(options.contains("version"))
		{
			printf("pushpin-proxy %s\n", VERSION);
			emit q->quit();
			return;
		}

		if(options.contains("verbose"))
			log_setOutputLevel(LOG_LEVEL_DEBUG);
		else
			log_setOutputLevel(LOG_LEVEL_INFO);

		QString logFile = options.value("logfile");
		if(!logFile.isEmpty())
		{
			if(!log_setFile(logFile))
			{
				log_error("failed to open log file: %s", qPrintable(logFile));
				emit q->quit();
				return;
			}
		}

		log_info("starting...");

		QString configFile = options.value("config");
		if(configFile.isEmpty())
			configFile = "/etc/pushpin/pushpin.conf";

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				log_error("failed to open %s, and --config not passed", qPrintable(configFile));
				emit q->quit();
				return;
			}
		}

		QSettings settings(configFile, QSettings::IniFormat);

		QStringList m2a_in_specs = settings.value("proxy/m2a_in_specs").toStringList();
		trimlist(&m2a_in_specs);
		QStringList m2a_in_stream_specs = settings.value("proxy/m2a_in_stream_specs").toStringList();
		trimlist(&m2a_in_stream_specs);
		QStringList m2a_out_specs = settings.value("proxy/m2a_out_specs").toStringList();
		trimlist(&m2a_out_specs);
		QStringList zurl_out_specs = settings.value("proxy/zurl_out_specs").toStringList();
		trimlist(&zurl_out_specs);
		QStringList zurl_out_stream_specs = settings.value("proxy/zurl_out_stream_specs").toStringList();
		trimlist(&zurl_out_stream_specs);
		QStringList zurl_in_specs = settings.value("proxy/zurl_in_specs").toStringList();
		trimlist(&zurl_in_specs);
		QString handler_inspect_spec = settings.value("proxy/handler_inspect_spec").toString();
		QString handler_retry_in_spec = settings.value("proxy/handler_retry_in_spec").toString();
		QString handler_accept_out_spec = settings.value("proxy/handler_accept_out_spec").toString();
		maxWorkers = settings.value("proxy/max_open_requests", -1).toInt();
		QString routesfile = settings.value("proxy/routesfile").toString();
		autoCrossOrigin = settings.value("proxy/auto_cross_origin").toBool();
		useXForwardedProtocol = settings.value("proxy/set_x_forwarded_protocol").toBool();
		xffRule = parse_xffRule(settings.value("proxy/x_forwarded_for").toStringList());
		xffTrustedRule = parse_xffRule(settings.value("proxy/x_forwarded_for_trusted").toStringList());
		QStringList origHeadersNeedMarkStr = settings.value("proxy/orig_headers_need_mark").toStringList();
		trimlist(&origHeadersNeedMarkStr);
		sigKey = parse_key(settings.value("proxy/sig_key").toString());
		upstreamKey = parse_key(settings.value("proxy/upstream_key").toString());

		origHeadersNeedMark.clear();
		foreach(const QString &s, origHeadersNeedMarkStr)
			origHeadersNeedMark += s.toUtf8();

		sigIss = "pushpin";

		// if routesfile is a relative path, then use it relative to the config file location
		QFileInfo fi(routesfile);
		if(fi.isRelative())
		{
			QString fname = fi.fileName();
			routesfile = QFileInfo(QDir(QFileInfo(configFile).absolutePath()), fname).filePath();
		}

		domainMap = new DomainMap(routesfile);

		if(m2a_in_specs.isEmpty() || m2a_in_stream_specs.isEmpty() || m2a_out_specs.isEmpty() || zurl_out_specs.isEmpty() || zurl_out_stream_specs.isEmpty() || zurl_in_specs.isEmpty())
		{
			log_error("must set m2a_in_specs, m2a_in_stream_specs, m2a_out_specs, zurl_out_specs, zurl_out_stream_specs, and zurl_in_specs");
			emit q->quit();
			return;
		}

		clientId = "pushpin-proxy_" + QByteArray::number(QCoreApplication::applicationPid());

		zhttp = new ZhttpManager(this);
		connect(zhttp, SIGNAL(requestReady()), SLOT(zhttp_requestReady()));

		zhttp->setInstanceId(clientId);

		zhttp->setServerInSpecs(m2a_in_specs);
		zhttp->setServerInStreamSpecs(m2a_in_stream_specs);
		zhttp->setServerOutSpecs(m2a_out_specs);

		zhttp->setClientOutSpecs(zurl_out_specs);
		zhttp->setClientOutStreamSpecs(zurl_out_stream_specs);
		zhttp->setClientInSpecs(zurl_in_specs);

		if(!handler_inspect_spec.isEmpty())
		{
			inspect = new InspectManager(this);
			if(!inspect->setSpec(handler_inspect_spec))
			{
				log_error("unable to bind to handler_inspect_spec: %s", qPrintable(handler_inspect_spec));
				emit q->quit();
				return;
			}

			inspectChecker = new InspectChecker(this);
		}

		if(!handler_retry_in_spec.isEmpty())
		{
			handler_retry_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);

			handler_retry_in_sock->setHwm(DEFAULT_HWM);

			if(!handler_retry_in_sock->bind(handler_retry_in_spec))
			{
				log_error("unable to bind to handler_retry_in_spec: %s", qPrintable(handler_retry_in_spec));
				emit q->quit();
				return;
			}

			handler_retry_in_valve = new QZmq::Valve(handler_retry_in_sock, this);
			connect(handler_retry_in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(handler_retry_in_readyRead(const QList<QByteArray> &)));
		}

		if(!handler_accept_out_spec.isEmpty())
		{
			handler_accept_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);

			handler_accept_out_sock->setHwm(DEFAULT_HWM);

			connect(handler_accept_out_sock, SIGNAL(messagesWritten(int)), SLOT(handler_accept_out_messagesWritten(int)));
			if(!handler_accept_out_sock->bind(handler_accept_out_spec))
			{
				log_error("unable to bind to handler_accept_out_spec: %s", qPrintable(handler_accept_out_spec));
				emit q->quit();
				return;
			}
		}

		if(handler_retry_in_valve)
			handler_retry_in_valve->open();

		log_info("started");
	}

	void doProxy(RequestSession *rs, const InspectData *idata = 0)
	{
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

			ps = new ProxySession(zhttp, domainMap, this);
			connect(ps, SIGNAL(addNotAllowed()), SLOT(ps_addNotAllowed()));
			connect(ps, SIGNAL(finishedByPassthrough()), SLOT(ps_finishedByPassthrough()));
			connect(ps, SIGNAL(finishedForAccept(const AcceptData &)), SLOT(ps_finishedForAccept(const AcceptData &)));
			connect(ps, SIGNAL(requestSessionDestroyed(RequestSession *)), SLOT(ps_requestSessionDestroyed(RequestSession *)));

			ps->setDefaultSigKey(sigIss, sigKey);
			ps->setDefaultUpstreamKey(upstreamKey);
			ps->setUseXForwardedProtocol(useXForwardedProtocol);
			ps->setXffRules(xffRule, xffTrustedRule);
			ps->setOrigHeadersNeedMark(origHeadersNeedMark);

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

		p.channelPrefix = adata.channelPrefix;

		QList<QByteArray> msg;
		msg += TnetString::fromVariant(p.toVariant());
		handler_accept_out_sock->write(msg);
	}

	void tryTakeRequest()
	{
		if(maxWorkers != -1 && requestSessions.count() >= maxWorkers)
			return;

		ZhttpRequest *req = zhttp->takeNext();
		if(!req)
			return;

		RequestSession *rs = new RequestSession(inspect, inspectChecker, this);
		connect(rs, SIGNAL(inspected(const InspectData &)), SLOT(rs_inspected(const InspectData &)));
		connect(rs, SIGNAL(inspectError()), SLOT(rs_inspectError()));
		connect(rs, SIGNAL(finished()), SLOT(rs_finished()));
		connect(rs, SIGNAL(finishedForAccept(const AcceptData &)), SLOT(rs_finishedForAccept(const AcceptData &)));

		rs->setAutoCrossOrigin(autoCrossOrigin);

		requestSessions += rs;

		rs->start(req);
	}

private slots:
	void m2_requestReady()
	{
		tryTakeRequest();
	}

	void zhttp_requestReady()
	{
		tryTakeRequest();
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

		requestSessions.remove(rs);
		delete rs;

		tryTakeRequest();
	}

	void rs_finishedForAccept(const AcceptData &adata)
	{
		RequestSession *rs = (RequestSession *)sender();

		if(!handler_accept_out_sock->canWriteImmediately())
		{
			rs->respondCannotAccept();
			return;
		}

		requestSessions.remove(rs);
		delete rs;

		sendAccept(adata);

		tryTakeRequest();
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

		tryTakeRequest();
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

		tryTakeRequest();
	}

	void ps_requestSessionDestroyed(RequestSession *rs)
	{
		requestSessions.remove(rs);

		tryTakeRequest();
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

			ZhttpRequest *zhttpRequest = zhttp->createFromState(ss);

			RequestSession *rs = new RequestSession(inspect, inspectChecker, this);
			rs->startRetry(zhttpRequest, req.autoCrossOrigin, req.jsonpCallback);

			requestSessions += rs;

			doProxy(rs, p.haveInspectInfo ? &idata : 0);
		}
	}

	void handler_accept_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void reload()
	{
		log_info("reloading");
		log_rotate();
		domainMap->reload();
	}

	void doQuit()
	{
		log_info("stopping...");

		// remove the handler, so if we get another signal then we crash out
		ProcessQuit::cleanup();

		log_info("stopped");
		emit q->quit();
	}
};

App::App(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

App::~App()
{
	delete d;
}

void App::start()
{
	d->start();
}

#include "app.moc"
