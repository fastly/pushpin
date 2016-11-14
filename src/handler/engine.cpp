/*
 * Copyright (C) 2015-2016 Fanout, Inc.
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
#include <algorithm>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "log.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "packet/retryrequestpacket.h"
#include "packet/wscontrolpacket.h"
#include "packet/statspacket.h"
#include "inspectdata.h"
#include "zutil.h"
#include "zrpcmanager.h"
#include "zrpcrequest.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"
#include "statsmanager.h"
#include "deferred.h"
#include "simplehttpserver.h"
#include "variantutil.h"
#include "detectrule.h"
#include "lastids.h"
#include "cidset.h"
#include "sessionrequest.h"
#include "requeststate.h"
#include "wscontrolmessage.h"
#include "publishformat.h"
#include "publishitem.h"
#include "jsonpointer.h"
#include "publishlastids.h"
#include "instruct.h"
#include "httpsession.h"
#include "controlrequest.h"
#include "conncheckworker.h"
#include "ratelimiter.h"

#define DEFAULT_HWM 101000
#define SUB_SNDHWM 0 // infinite
#define RETRY_WAIT_TIME 0
#define WSCONTROL_WAIT_TIME 0
#define STATE_RPC_TIMEOUT 1000
#define PROXY_RPC_TIMEOUT 10000
#define DEFAULT_WS_KEEPALIVE_TIMEOUT 55
#define SUBSCRIBED_DELAY 1000

#define INSPECT_WORKERS_MAX 10
#define ACCEPT_WORKERS_MAX 10

using namespace VariantUtil;

// return true to send and false to drop.
// TODO: support more than one filter, payload modification, etc
static bool applyFilters(const QHash<QString, QString> &subscriptionMeta, const QHash<QString, QString> &publishMeta, const QStringList &filters)
{
	foreach(const QString &f, filters)
	{
		if(f == "skip-self")
		{
			QString user = subscriptionMeta.value("user");
			QString sender = publishMeta.value("sender");
			if(!user.isEmpty() && !sender.isEmpty() && sender == user)
				return false;
		}
	}

	return true;
}

static QList<PublishItem> parseHttpItems(const QVariantList &vitems, bool *ok = 0, QString *errorMessage = 0)
{
	QList<PublishItem> out;

	foreach(const QVariant &vitem, vitems)
	{
		bool ok_;
		PublishItem item = PublishItem::fromVariant(vitem, QString(), &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return QList<PublishItem>();
		}

		out += item;
	}

	setSuccess(ok, errorMessage);
	return out;
}

class InspectWorker : public Deferred
{
	Q_OBJECT

public:
	ZrpcRequest *req;
	ZrpcManager *stateClient;
	bool shareAll;
	HttpRequestData requestData;
	bool truncated;
	bool autoShare;
	QString sid;
	LastIds lastIds;

	InspectWorker(ZrpcRequest *_req, ZrpcManager *_stateClient, bool _shareAll, QObject *parent = 0) :
		Deferred(parent),
		req(_req),
		stateClient(_stateClient),
		shareAll(_shareAll),
		truncated(false),
		autoShare(false)
	{
		req->setParent(this);

		if(req->method() == "inspect")
		{
			QVariantHash args = req->args();

			if(!args.contains("method") || args["method"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.method = QString::fromLatin1(args["method"].toByteArray());

			if(!args.contains("uri") || args["uri"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.uri = QUrl(args["uri"].toString(), QUrl::StrictMode);
			if(!requestData.uri.isValid())
			{
				respondError("bad-request");
				return;
			}

			if(!args.contains("headers") || args["headers"].type() != QVariant::List)
			{
				respondError("bad-request");
				return;
			}

			foreach(const QVariant &vheader, args["headers"].toList())
			{
				if(vheader.type() != QVariant::List)
				{
					respondError("bad-request");
					return;
				}

				QVariantList vlist = vheader.toList();
				if(vlist.count() != 2 || vlist[0].type() != QVariant::ByteArray || vlist[1].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				requestData.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
			}

			if(!args.contains("body") || args["body"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.body = args["body"].toByteArray();

			truncated = false;
			if(args.contains("truncated"))
			{
				if(args["truncated"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				truncated = args["truncated"].toBool();
			}

			bool getSession = false;
			if(args.contains("get-session"))
			{
				if(args["get-session"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				getSession = args["get-session"].toBool();
			}

			autoShare = false;
			if(args.contains("auto-share"))
			{
				if(args["auto-share"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				autoShare = args["auto-share"].toBool();
			}

			if(getSession && stateClient)
			{
				// determine session info
				Deferred *d = SessionRequest::detectRulesGet(stateClient, requestData.uri.host().toUtf8(), requestData.uri.path(QUrl::FullyEncoded).toUtf8(), this);
				connect(d, &Deferred::finished, this, &InspectWorker::sessionDetectRulesGet_finished);
				return;
			}

			doFinish();
		}
		else
		{
			respondError("method-not-found");
		}
	}

private:
	void respondError(const QByteArray &condition)
	{
		req->respondError(condition);
		setFinished(true);
	}

	void doFinish()
	{
		QVariantHash result;
		result["no-proxy"] = false;

		if(autoShare && requestData.method == "GET")
		{
			// auto share matches requests based on URI path (not query) and
			//   Grip-Last headers. the reason the query part is not
			//   considered is because it may vary per client and Grip-Last
			//   supersedes whatever is in the query

			QUrl uri = requestData.uri;
			uri.setQuery(QString()); // remove the query part

			QList<QByteArray> gripLastHeaders = requestData.headers.getAll("Grip-Last");
			std::sort(gripLastHeaders.begin(), gripLastHeaders.end());

			QByteArray key = "auto|" + uri.toEncoded();

			foreach(const QByteArray &h, gripLastHeaders)
				key += '|' + h;

			result["sharing-key"] = key;
		}
		else if(shareAll)
			result["sharing-key"] = requestData.method.toLatin1() + '|' + requestData.uri.toEncoded();

		if(!sid.isEmpty())
		{
			result["sid"] = sid.toUtf8();

			if(!lastIds.isEmpty())
			{
				QVariantHash vlastIds;
				QHashIterator<QString, QString> it(lastIds);
				while(it.hasNext())
				{
					it.next();
					vlastIds.insert(it.key(), it.value().toUtf8());
				}

				result["last-ids"] = vlastIds;
			}
		}

		req->respond(result);
		setFinished(true);
	}

private slots:
	void sessionDetectRulesGet_finished(const DeferredResult &result)
	{
		if(result.success)
		{
			QList<DetectRule> rules = result.value.value<DetectRuleList>();
			log_debug("retrieved %d rules", rules.count());

			foreach(const DetectRule &rule, rules)
			{
				QByteArray jsonData;

				if(!rule.jsonParam.isEmpty())
				{
					QUrlQuery tmp(QString::fromUtf8(requestData.body));
					jsonData = tmp.queryItemValue(rule.jsonParam).toUtf8();
				}
				else
				{
					jsonData = requestData.body;
				}

				QJsonParseError e;
				QJsonDocument doc = QJsonDocument::fromJson(jsonData, &e);
				if(e.error != QJsonParseError::NoError)
					continue;

				QVariant vdata;
				if(doc.isObject())
					vdata = doc.object().toVariantMap();
				else if(doc.isArray())
					vdata = doc.array().toVariantList();
				else
					continue;

				JsonPointer ptr = JsonPointer::resolve(&vdata, rule.sidPtr);
				if(!ptr.isNull() && ptr.exists())
				{
					bool ok;
					sid = getString(ptr.value(), &ok);
					if(!ok)
						continue;

					break;
				}
			}

			if(!sid.isEmpty())
			{
				Deferred *d = SessionRequest::getLastIds(stateClient, sid, this);
				connect(d, &Deferred::finished, this, &InspectWorker::sessionGetLastIds_finished);
				return;
			}
		}
		else
		{
			// log error but keep going
			log_error("failed to detect session: condition=%d", result.value.toInt());
		}

		doFinish();
	}

	void sessionGetLastIds_finished(const DeferredResult &result)
	{
		if(result.success)
		{
			lastIds = result.value.value<LastIds>();
		}
		else
		{
			QByteArray errorCondition = result.value.toByteArray();

			if(errorCondition != "item-not-found")
			{
				// log error but keep going
				log_error("failed to detect session: condition=%d", result.value.toInt());
			}
		}

		doFinish();
	}
};

class WsSession : public QObject
{
	Q_OBJECT

public:
	QString cid;
	QString channelPrefix;
	HttpRequestData requestData;
	QString route;
	QString sid;
	QHash<QString, QString> meta;
	QHash<QString, QStringList> channelFilters; // k=channel, v=list(filters)
	QSet<QString> channels;
	int ttl;
	QByteArray keepAliveType;
	QByteArray keepAliveMessage;
	QTimer *timer;

	WsSession(QObject *parent = 0) :
		QObject(parent)
	{
		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, &WsSession::timer_timeout);
	}

	~WsSession()
	{
		timer->disconnect(this);
		timer->setParent(0);
		timer->deleteLater();
	}

	void refreshExpiration()
	{
		timer->start(ttl * 1000);
	}

signals:
	void expired();

private slots:
	void timer_timeout()
	{
		log_debug("timing out ws session: %s", qPrintable(cid));

		emit expired();
	}
};

class Subscription;

class CommonState
{
public:
	QHash<ZhttpRequest::Rid, HttpSession*> httpSessions;
	QHash<QString, WsSession*> wsSessions;
	QHash<QString, QSet<HttpSession*> > responseSessionsByChannel;
	QHash<QString, QSet<HttpSession*> > streamSessionsByChannel;
	QHash<QString, QSet<WsSession*> > wsSessionsByChannel;
	PublishLastIds responseLastIds;
	PublishLastIds streamLastIds;
	QHash<QString, Subscription*> subs;

	CommonState() :
		responseLastIds(1000000),
		streamLastIds(1000000)
	{
	}

	QSet<QString> removeWsSessionChannels(WsSession *s)
	{
		QSet<QString> out;

		foreach(const QString &channel, s->channels)
		{
			if(!wsSessionsByChannel.contains(channel))
				continue;

			QSet<WsSession*> &cur = wsSessionsByChannel[channel];
			if(!cur.contains(s))
				continue;

			cur.remove(s);

			if(cur.isEmpty())
			{
				wsSessionsByChannel.remove(channel);
				out += channel;
			}
		}

		return out;
	}
};

class AcceptWorker : public Deferred
{
	Q_OBJECT

public:
	ZrpcRequest *req;
	ZrpcManager *stateClient;
	CommonState *cs;
	ZhttpManager *zhttpIn;
	ZhttpManager *zhttpOut;
	StatsManager *stats;
	RateLimiter *updateLimiter;
	QString route;
	QString channelPrefix;
	QByteArray sigIss;
	QByteArray sigKey;
	bool trusted;
	QHash<ZhttpRequest::Rid, RequestState> requestStates;
	HttpRequestData requestData;
	bool haveInspectInfo;
	InspectData inspectInfo;
	HttpResponseData responseData;
	bool responseSent;
	QString sid;
	LastIds lastIds;
	QList<HttpSession*> sessions;

	AcceptWorker(ZrpcRequest *_req, ZrpcManager *_stateClient, CommonState *_cs, ZhttpManager *_zhttpIn, ZhttpManager *_zhttpOut, StatsManager *_stats, RateLimiter *_updateLimiter, QObject *parent = 0) :
		Deferred(parent),
		req(_req),
		stateClient(_stateClient),
		cs(_cs),
		zhttpIn(_zhttpIn),
		zhttpOut(_zhttpOut),
		stats(_stats),
		updateLimiter(_updateLimiter),
		trusted(false),
		haveInspectInfo(false),
		responseSent(false)
	{
		req->setParent(this);
	}

	void start()
	{
		if(req->method() == "accept")
		{
			QVariantHash args = req->args();

			if(args.contains("route"))
			{
				if(args["route"].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				route = QString::fromUtf8(args["route"].toByteArray());
			}

			if(args.contains("channel-prefix"))
			{
				if(args["channel-prefix"].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				channelPrefix = QString::fromUtf8(args["channel-prefix"].toByteArray());
			}

			if(args.contains("sig-iss"))
			{
				if(args["sig-iss"].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				sigIss = args["sig-iss"].toByteArray();
			}

			if(args.contains("sig-key"))
			{
				if(args["sig-key"].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				sigKey = args["sig-key"].toByteArray();
			}

			if(args.contains("trusted"))
			{
				if(args["trusted"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				trusted = args["trusted"].toBool();
			}

			// parse requests

			if(!args.contains("requests") || args["requests"].type() != QVariant::List)
			{
				respondError("bad-request");
				return;
			}

			foreach(const QVariant &vr, args["requests"].toList())
			{
				RequestState rs = RequestState::fromVariant(vr);
				if(rs.rid.first.isEmpty())
				{
					respondError("bad-request");
					return;
				}

				requestStates.insert(rs.rid, rs);
			}

			// parse request-data

			if(!args.contains("request-data") || args["request-data"].type() != QVariant::Hash)
			{
				respondError("bad-request");
				return;
			}

			QVariantHash rd = args["request-data"].toHash();

			if(!rd.contains("method") || rd["method"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.method = QString::fromLatin1(rd["method"].toByteArray());

			if(!rd.contains("uri") || rd["uri"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.uri = QUrl(rd["uri"].toString(), QUrl::StrictMode);
			if(!requestData.uri.isValid())
			{
				respondError("bad-request");
				return;
			}

			if(!rd.contains("headers") || rd["headers"].type() != QVariant::List)
			{
				respondError("bad-request");
				return;
			}

			foreach(const QVariant &vheader, rd["headers"].toList())
			{
				if(vheader.type() != QVariant::List)
				{
					respondError("bad-request");
					return;
				}

				QVariantList vlist = vheader.toList();
				if(vlist.count() != 2 || vlist[0].type() != QVariant::ByteArray || vlist[1].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				requestData.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
			}

			if(!rd.contains("body") || rd["body"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.body = rd["body"].toByteArray();

			// parse response

			if(!args.contains("response") || args["response"].type() != QVariant::Hash)
			{
				respondError("bad-request");
				return;
			}

			rd = args["response"].toHash();

			if(!rd.contains("code") || !rd["code"].canConvert(QVariant::Int))
			{
				respondError("bad-request");
				return;
			}

			responseData.code = rd["code"].toInt();

			if(!rd.contains("reason") || rd["reason"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			responseData.reason = rd["reason"].toByteArray();

			if(!rd.contains("headers") || rd["headers"].type() != QVariant::List)
			{
				respondError("bad-request");
				return;
			}

			foreach(const QVariant &vheader, rd["headers"].toList())
			{
				if(vheader.type() != QVariant::List)
				{
					respondError("bad-request");
					return;
				}

				QVariantList vlist = vheader.toList();
				if(vlist.count() != 2 || vlist[0].type() != QVariant::ByteArray || vlist[1].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				responseData.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
			}

			if(!rd.contains("body") || rd["body"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			responseData.body = rd["body"].toByteArray();

			if(args.contains("inspect"))
			{
				if(args["inspect"].type() != QVariant::Hash)
				{
					respondError("bad-request");
					return;
				}

				QVariantHash vinspect = args["inspect"].toHash();

				if(!vinspect.contains("no-proxy") || vinspect["no-proxy"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				inspectInfo.doProxy = !vinspect["no-proxy"].toBool();

				inspectInfo.sharingKey.clear();
				if(vinspect.contains("sharing-key"))
				{
					if(vinspect["sharing-key"].type() != QVariant::ByteArray)
					{
						respondError("bad-request");
						return;
					}

					inspectInfo.sharingKey = vinspect["sharing-key"].toByteArray();
				}

				if(vinspect.contains("sid"))
				{
					if(vinspect["sid"].type() != QVariant::ByteArray)
					{
						respondError("bad-request");
						return;
					}

					inspectInfo.sid = vinspect["sid"].toByteArray();
				}

				if(vinspect.contains("last-ids"))
				{
					if(vinspect["last-ids"].type() != QVariant::Hash)
					{
						respondError("bad-request");
						return;
					}

					QVariantHash vlastIds = vinspect["last-ids"].toHash();
					QHashIterator<QString, QVariant> it(vlastIds);
					while(it.hasNext())
					{
						it.next();

						if(it.value().type() != QVariant::ByteArray)
						{
							respondError("bad-request");
							return;
						}

						QByteArray key = it.key().toUtf8();
						QByteArray val = it.value().toByteArray();
						inspectInfo.lastIds.insert(key, val);
					}
				}

				inspectInfo.userData = vinspect["user-data"];

				haveInspectInfo = true;
			}

			if(args.contains("response-sent"))
			{
				if(args["response-sent"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				responseSent = args["response-sent"].toBool();
			}

			bool useSession = false;
			if(args.contains("use-session"))
			{
				if(args["use-session"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				useSession = args["use-session"].toBool();
			}

			sid = QString::fromUtf8(responseData.headers.get("Grip-Session-Id"));

			QList<DetectRule> rules;
			QList<HttpHeaderParameters> ruleHeaders = responseData.headers.getAllAsParameters("Grip-Session-Detect", HttpHeaders::ParseAllParameters);
			foreach(const HttpHeaderParameters &params, ruleHeaders)
			{
				if(params.contains("path-prefix") && params.contains("sid-ptr"))
				{
					DetectRule rule;
					rule.domain = requestData.uri.host();
					rule.pathPrefix = params.get("path-prefix");
					rule.sidPtr = QString::fromUtf8(params.get("sid-ptr"));
					if(params.contains("json-param"))
						rule.jsonParam = QString::fromUtf8(params.get("json-param"));
					rules += rule;
				}
			}

			QList<HttpHeaderParameters> lastHeaders = responseData.headers.getAllAsParameters("Grip-Last");
			foreach(const HttpHeaderParameters &params, lastHeaders)
			{
				lastIds.insert(params[0].first, params.get("last-id"));
			}

			if(useSession && stateClient)
			{
				if(!rules.isEmpty())
				{
					Deferred *d = SessionRequest::detectRulesSet(stateClient, rules, this);
					connect(d, &Deferred::finished, this, &AcceptWorker::sessionDetectRulesSet_finished);
				}
				else
				{
					afterSetRules();
				}

				return;
			}

			afterSessionCalls();
		}
		else
		{
			respondError("method-not-found");
		}
	}

	QList<HttpSession*> takeSessions()
	{
		QList<HttpSession*> out = sessions;
		sessions.clear();

		foreach(HttpSession *hs, out)
			hs->setParent(0);

		return out;
	}

signals:
	void sessionsReady();
	void retryPacketReady(const RetryRequestPacket &packet);

private:
	void respondError(const QByteArray &condition, const QVariant &result = QVariant())
	{
		req->respondError(condition, result);
		setFinished(true);
	}

	void afterSetRules()
	{
		if(!sid.isEmpty())
		{
			Deferred *d = SessionRequest::createOrUpdate(stateClient, sid, lastIds, this);
			connect(d, &Deferred::finished, this, &AcceptWorker::sessionCreateOrUpdate_finished);
		}
		else
		{
			afterSessionCalls();
		}
	}

	void afterSessionCalls()
	{
		bool ok;
		QString errorMessage;
		Instruct instruct = Instruct::fromResponse(responseData, &ok, &errorMessage);
		if(!ok)
		{
			respondError("bad-format", errorMessage.toUtf8());
			return;
		}

		// don't relay these headers. their meaning is handled by
		//   zurl and they only apply to the outgoing hop.
		instruct.response.headers.removeAll("Connection");
		instruct.response.headers.removeAll("Keep-Alive");
		instruct.response.headers.removeAll("Content-Encoding");
		instruct.response.headers.removeAll("Transfer-Encoding");

		if(instruct.holdMode == Instruct::NoHold && instruct.nextLink.isEmpty())
		{
			QVariantHash result;

			if(!responseSent)
			{
				QVariantHash vresponse;
				vresponse["code"] = instruct.response.code;
				vresponse["reason"] = instruct.response.reason;
				QVariantList vheaders;
				foreach(const HttpHeader &h, instruct.response.headers)
				{
					QVariantList vheader;
					vheader += h.first;
					vheader += h.second;
					vheaders += QVariant(vheader);
				}
				vresponse["headers"] = vheaders;
				vresponse["body"] = instruct.response.body;

				result["response"] = vresponse;
			}

			req->respond(result);

			setFinished(true);
			return;
		}

		QVariantHash result;
		result["accepted"] = true;
		req->respond(result);

		log_debug("accepting %d requests", requestStates.count());

		if(instruct.holdMode == Instruct::ResponseHold)
		{
			bool conflict = false;
			foreach(const Instruct::Channel &c, instruct.channels)
			{
				if(!c.prevId.isNull())
				{
					QString name = channelPrefix + c.name;

					QString lastId = cs->responseLastIds.value(name);
					if(!lastId.isNull() && lastId != c.prevId)
					{
						log_debug("lastid inconsistency (got=%s, expected=%s), retrying", qPrintable(c.prevId), qPrintable(lastId));
						cs->responseLastIds.remove(name);
						conflict = true;

						// NOTE: don't exit loop here. we want to clear
						//   the last ids of all conflicting channels
					}
				}
			}

			if(conflict)
			{
				RetryRequestPacket rp;

				foreach(const RequestState &rs, requestStates)
				{
					RetryRequestPacket::Request rpreq;
					rpreq.rid = rs.rid;
					rpreq.https = rs.isHttps;
					rpreq.peerAddress = rs.peerAddress;
					rpreq.debug = rs.debug;
					rpreq.autoCrossOrigin = rs.autoCrossOrigin;
					rpreq.jsonpCallback = rs.jsonpCallback;
					rpreq.jsonpExtendedResponse = rs.jsonpExtendedResponse;
					rpreq.inSeq = rs.inSeq;
					rpreq.outSeq = rs.outSeq;
					rpreq.outCredits = rs.outCredits;
					rpreq.userData = rs.userData;

					rp.requests += rpreq;
				}

				rp.requestData = requestData;

				if(haveInspectInfo)
				{
					rp.haveInspectInfo = true;
					rp.inspectInfo.doProxy = inspectInfo.doProxy;
					rp.inspectInfo.sharingKey = inspectInfo.sharingKey;
					rp.inspectInfo.sid = inspectInfo.sid;
					rp.inspectInfo.lastIds = inspectInfo.lastIds;
					rp.inspectInfo.userData = inspectInfo.userData;
				}

				emit retryPacketReady(rp);

				setFinished(true);
				return;
			}
		}

		foreach(const RequestState &rs, requestStates)
		{
			ZhttpRequest::Rid rid(rs.rid.first, rs.rid.second);

			if(zhttpIn->serverRequestByRid(rid))
			{
				log_error("received accept request for rid we already have (%s, %s), skipping", rid.first.data(), rid.second.data());
				continue;
			}

			ZhttpRequest::ServerState ss;
			ss.rid = ZhttpRequest::Rid(rs.rid.first, rs.rid.second);
			ss.peerAddress = rs.peerAddress;
			ss.requestMethod = requestData.method;
			ss.requestUri = requestData.uri;
			ss.requestUri.setScheme(rs.isHttps ? "https" : "http");
			ss.requestHeaders = requestData.headers;
			ss.requestBody = requestData.body;
			ss.responseCode = rs.responseCode;
			ss.inSeq = rs.inSeq;
			ss.outSeq = rs.outSeq;
			ss.outCredits = rs.outCredits;
			ss.userData = rs.userData;

			// take over responsibility for request
			ZhttpRequest *httpReq = zhttpIn->createRequestFromState(ss);

			HttpSession::AcceptData adata;
			adata.requestData = requestData;
			adata.peerAddress = rs.peerAddress;
			adata.debug = rs.debug;
			adata.autoCrossOrigin = rs.autoCrossOrigin;
			adata.jsonpCallback = rs.jsonpCallback;
			adata.jsonpExtendedResponse = rs.jsonpExtendedResponse;
			adata.route = route;
			adata.channelPrefix = channelPrefix;
			adata.sid = sid;
			adata.responseSent = responseSent;
			adata.sigIss = sigIss;
			adata.sigKey = sigKey;
			adata.trusted = trusted;

			PublishLastIds &publishLastIds = (instruct.holdMode == Instruct::ResponseHold ? cs->responseLastIds : cs->streamLastIds);

			sessions += new HttpSession(httpReq, adata, instruct, zhttpOut, stats, updateLimiter, &publishLastIds, this);
		}

		// engine should directly connect to this and register the holds
		//   immediately, to avoid a race with the lastId check
		emit sessionsReady();

		setFinished(true);
	}

private slots:
	void sessionDetectRulesSet_finished(const DeferredResult &result)
	{
		if(!result.success)
			log_error("couldn't store detection rules: condition=%d", result.value.toInt());

		afterSetRules();
	}

	void sessionCreateOrUpdate_finished(const DeferredResult &result)
	{
		if(!result.success)
			log_error("couldn't create/update session: condition=%d", result.value.toInt());

		afterSessionCalls();
	}
};

class Subscription : public QObject
{
	Q_OBJECT

public:
	Subscription(const QString &channel) :
		channel_(channel),
		timer_(0)
	{
	}

	~Subscription()
	{
		if(timer_)
		{
			timer_->stop();
			timer_->disconnect(this);
			timer_->setParent(0);
			timer_->deleteLater();
		}
	}

	const QString & channel() const
	{
		return channel_;
	}

	void start()
	{
		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, this, &Subscription::timer_timeout);
		timer_->setSingleShot(true);
		timer_->start(SUBSCRIBED_DELAY);
	}

signals:
	void subscribed();

private:
	QString channel_;
	QTimer *timer_;

private slots:
	void timer_timeout()
	{
		emit subscribed();
	}
};

class Engine::Private : public QObject
{
	Q_OBJECT

public:
	class PublishAction : public RateLimiter::Action
	{
	public:
		Engine::Private *ep;
		QPointer<QObject> target;
		PublishItem item;
		QList<QByteArray> exposeHeaders;

		PublishAction(Engine::Private *_ep, QObject *_target, const PublishItem &_item, const QList<QByteArray> &_exposeHeaders = QList<QByteArray>()) :
			ep(_ep),
			target(_target),
			item(_item),
			exposeHeaders(_exposeHeaders)
		{
		}

		virtual bool execute()
		{
			if(!target)
				return false;

			ep->publishSend(target, item, exposeHeaders);
			return true;
		}
	};

	Engine *q;
	Configuration config;
	ZhttpManager *zhttpIn;
	ZhttpManager *zhttpOut;
	ZrpcManager *inspectServer;
	ZrpcManager *acceptServer;
	ZrpcManager *stateClient;
	ZrpcManager *controlServer;
	ZrpcManager *proxyControlClient;
	QZmq::Socket *inPullSock;
	QZmq::Valve *inPullValve;
	QZmq::Socket *inSubSock;
	QZmq::Valve *inSubValve;
	QZmq::Socket *retrySock;
	QZmq::Socket *wsControlInSock;
	QZmq::Valve *wsControlInValve;
	QZmq::Socket *wsControlOutSock;
	QZmq::Socket *statsSock;
	QZmq::Socket *proxyStatsSock;
	QZmq::Valve *proxyStatsValve;
	SimpleHttpServer *controlHttpServer;
	StatsManager *stats;
	RateLimiter *publishLimiter;
	RateLimiter *updateLimiter;
	CommonState cs;
	QSet<InspectWorker*> inspectWorkers;
	QSet<AcceptWorker*> acceptWorkers;
	QSet<Deferred*> deferreds;
	Deferred *report;

	Private(Engine *_q) :
		QObject(_q),
		q(_q),
		zhttpIn(0),
		zhttpOut(0),
		inspectServer(0),
		acceptServer(0),
		stateClient(0),
		controlServer(0),
		proxyControlClient(0),
		inPullSock(0),
		inPullValve(0),
		inSubSock(0),
		inSubValve(0),
		retrySock(0),
		wsControlInSock(0),
		wsControlInValve(0),
		wsControlOutSock(0),
		statsSock(0),
		proxyStatsSock(0),
		proxyStatsValve(0),
		controlHttpServer(0),
		stats(0),
		report(0)
	{
		qRegisterMetaType<DetectRuleList>();

		publishLimiter = new RateLimiter(this);

		updateLimiter = new RateLimiter(this);
	}

	~Private()
	{
		qDeleteAll(inspectWorkers);
		qDeleteAll(acceptWorkers);
		qDeleteAll(deferreds);
		qDeleteAll(cs.wsSessions);
		qDeleteAll(cs.httpSessions);
		qDeleteAll(cs.subs);
	}

	bool start(const Configuration &_config)
	{
		config = _config;

		publishLimiter->setRate(config.messageRate);
		publishLimiter->setHwm(config.messageHwm);

		updateLimiter->setRate(10);
		updateLimiter->setBatchWaitEnabled(true);

		zhttpIn = new ZhttpManager(this);
		zhttpIn->setInstanceId(config.instanceId);
		zhttpIn->setServerInStreamSpecs(config.serverInStreamSpecs);
		zhttpIn->setServerOutSpecs(config.serverOutSpecs);

		zhttpOut = new ZhttpManager(this);
		zhttpOut->setInstanceId(config.instanceId);
		zhttpOut->setClientOutSpecs(config.clientOutSpecs);
		zhttpOut->setClientOutStreamSpecs(config.clientOutStreamSpecs);
		zhttpOut->setClientInSpecs(config.clientInSpecs);

		log_info("zhttp in stream: %s", qPrintable(config.serverInStreamSpecs.join(", ")));
		log_info("zhttp out: %s", qPrintable(config.serverOutSpecs.join(", ")));

		if(!config.inspectSpec.isEmpty())
		{
			inspectServer = new ZrpcManager(this);
			inspectServer->setBind(false);
			inspectServer->setIpcFileMode(config.ipcFileMode);
			connect(inspectServer, &ZrpcManager::requestReady, this, &Private::inspectServer_requestReady);

			if(!inspectServer->setServerSpecs(QStringList() << config.inspectSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("inspect server: %s", qPrintable(config.inspectSpec));
		}

		if(!config.acceptSpec.isEmpty())
		{
			acceptServer = new ZrpcManager(this);
			acceptServer->setBind(false);
			acceptServer->setIpcFileMode(config.ipcFileMode);
			connect(acceptServer, &ZrpcManager::requestReady, this, &Private::acceptServer_requestReady);

			if(!acceptServer->setServerSpecs(QStringList() << config.acceptSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("accept server: %s", qPrintable(config.acceptSpec));
		}

		if(!config.stateSpec.isEmpty())
		{
			stateClient = new ZrpcManager(this);
			stateClient->setBind(true);
			stateClient->setIpcFileMode(config.ipcFileMode);
			stateClient->setTimeout(STATE_RPC_TIMEOUT);

			if(!stateClient->setClientSpecs(QStringList() << config.stateSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("state client: %s", qPrintable(config.stateSpec));
		}

		if(!config.commandSpec.isEmpty())
		{
			controlServer = new ZrpcManager(this);
			controlServer->setBind(true);
			controlServer->setIpcFileMode(config.ipcFileMode);
			connect(controlServer, &ZrpcManager::requestReady, this, &Private::controlServer_requestReady);

			if(!controlServer->setServerSpecs(QStringList() << config.commandSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("control server: %s", qPrintable(config.commandSpec));
		}

		if(!config.pushInSpec.isEmpty())
		{
			inPullSock = new QZmq::Socket(QZmq::Socket::Pull, this);
			inPullSock->setHwm(DEFAULT_HWM);

			QString errorMessage;
			if(!ZUtil::setupSocket(inPullSock, config.pushInSpec, true, config.ipcFileMode, &errorMessage))
			{
					log_error("%s", qPrintable(errorMessage));
					return false;
			}

			inPullValve = new QZmq::Valve(inPullSock, this);
			connect(inPullValve, &QZmq::Valve::readyRead, this, &Private::inPull_readyRead);

			log_info("in pull: %s", qPrintable(config.pushInSpec));
		}

		if(!config.pushInSubSpec.isEmpty())
		{
			inSubSock = new QZmq::Socket(QZmq::Socket::Sub, this);
			inSubSock->setSendHwm(SUB_SNDHWM);
			inSubSock->setShutdownWaitTime(0);

			QString errorMessage;
			if(!ZUtil::setupSocket(inSubSock, config.pushInSubSpec, true, config.ipcFileMode, &errorMessage))
			{
					log_error("%s", qPrintable(errorMessage));
					return false;
			}

			inSubValve = new QZmq::Valve(inSubSock, this);
			connect(inSubValve, &QZmq::Valve::readyRead, this, &Private::inSub_readyRead);

			log_info("in sub: %s", qPrintable(config.pushInSubSpec));
		}

		if(!config.retryOutSpec.isEmpty())
		{
			retrySock = new QZmq::Socket(QZmq::Socket::Push, this);
			retrySock->setHwm(DEFAULT_HWM);
			retrySock->setShutdownWaitTime(RETRY_WAIT_TIME);

			QString errorMessage;
			if(!ZUtil::setupSocket(retrySock, config.retryOutSpec, false, config.ipcFileMode, &errorMessage))
			{
					log_error("%s", qPrintable(errorMessage));
					return false;
			}

			log_info("retry: %s", qPrintable(config.retryOutSpec));
		}

		if(!config.wsControlInSpec.isEmpty() && !config.wsControlOutSpec.isEmpty())
		{
			wsControlInSock = new QZmq::Socket(QZmq::Socket::Pull, this);
			wsControlInSock->setHwm(DEFAULT_HWM);

			QString errorMessage;
			if(!ZUtil::setupSocket(wsControlInSock, config.wsControlInSpec, false, config.ipcFileMode, &errorMessage))
			{
					log_error("%s", qPrintable(errorMessage));
					return false;
			}

			wsControlInValve = new QZmq::Valve(wsControlInSock, this);
			connect(wsControlInValve, &QZmq::Valve::readyRead, this, &Private::wsControlIn_readyRead);

			log_info("ws control in: %s", qPrintable(config.wsControlInSpec));

			wsControlOutSock = new QZmq::Socket(QZmq::Socket::Push, this);
			wsControlOutSock->setHwm(DEFAULT_HWM);
			wsControlOutSock->setShutdownWaitTime(WSCONTROL_WAIT_TIME);

			if(!ZUtil::setupSocket(wsControlOutSock, config.wsControlOutSpec, false, config.ipcFileMode, &errorMessage))
			{
					log_error("%s", qPrintable(errorMessage));
					return false;
			}

			log_info("ws control out: %s", qPrintable(config.wsControlOutSpec));
		}

		stats = new StatsManager(this);
		connect(stats, &StatsManager::connectionsRefreshed, this, &Private::stats_connectionsRefreshed);
		connect(stats, &StatsManager::unsubscribed, this, &Private::stats_unsubscribed);
		connect(stats, &StatsManager::reported, this, &Private::stats_reported);

		stats->setReportsEnabled(true);

		if(!config.statsSpec.isEmpty())
		{
			stats->setInstanceId(config.instanceId);
			stats->setIpcFileMode(config.ipcFileMode);

			if(!stats->setSpec(config.statsSpec))
			{
				// statsmanager logs error
				return false;
			}

			log_info("stats: %s", qPrintable(config.statsSpec));
		}

		if(!config.proxyStatsSpec.isEmpty())
		{
			proxyStatsSock = new QZmq::Socket(QZmq::Socket::Sub, this);
			proxyStatsSock->setHwm(DEFAULT_HWM);
			proxyStatsSock->setShutdownWaitTime(0);
			proxyStatsSock->subscribe("");

			QString errorMessage;
			if(!ZUtil::setupSocket(proxyStatsSock, config.proxyStatsSpec, false, config.ipcFileMode, &errorMessage))
			{
					log_error("%s", qPrintable(errorMessage));
					return false;
			}

			proxyStatsValve = new QZmq::Valve(proxyStatsSock, this);
			connect(proxyStatsValve, &QZmq::Valve::readyRead, this, &Private::proxyStats_readyRead);

			log_info("proxy stats: %s", qPrintable(config.proxyStatsSpec));
		}

		if(!config.proxyCommandSpec.isEmpty())
		{
			proxyControlClient = new ZrpcManager(this);
			proxyControlClient->setIpcFileMode(config.ipcFileMode);
			proxyControlClient->setTimeout(PROXY_RPC_TIMEOUT);

			if(!proxyControlClient->setClientSpecs(QStringList() << config.proxyCommandSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("proxy control client: %s", qPrintable(config.proxyCommandSpec));
		}

		if(config.pushInHttpPort != -1)
		{
			controlHttpServer = new SimpleHttpServer(this);
			connect(controlHttpServer, &SimpleHttpServer::requestReady, this, &Private::controlHttpServer_requestReady);
			controlHttpServer->listen(config.pushInHttpAddr, config.pushInHttpPort);

			log_info("http control server: %s:%d", qPrintable(config.pushInHttpAddr.toString()), config.pushInHttpPort);
		}

		if(inPullValve)
			inPullValve->open();
		if(inSubValve)
			inSubValve->open();
		if(wsControlInValve)
			wsControlInValve->open();
		if(proxyStatsValve)
			proxyStatsValve->open();

		return true;
	}

	void reload()
	{
		// nothing to do
	}

private:
	void handlePublishItem(const PublishItem &item)
	{
		// always add for non-identified route
		stats->addMessageReceived(QByteArray());

		QList<HttpSession*> responseSessions;
		QList<HttpSession*> streamSessions;
		QList<WsSession*> wsSessions;
		QSet<QString> sids;

		if(item.formats.contains(PublishFormat::HttpResponse))
		{
			QSet<HttpSession*> sessions = cs.responseSessionsByChannel.value(item.channel);
			foreach(HttpSession *hs, sessions)
			{
				assert(hs->holdMode() == Instruct::ResponseHold);
				assert(hs->channels().contains(item.channel));

				if(!applyFilters(hs->meta(), item.meta, hs->channels()[item.channel].filters))
					continue;

				responseSessions += hs;

				if(!hs->sid().isEmpty())
					sids += hs->sid();
			}

			if(!item.id.isNull())
				cs.responseLastIds.set(item.channel, item.id);
		}

		if(item.formats.contains(PublishFormat::HttpStream))
		{
			QSet<HttpSession*> sessions = cs.streamSessionsByChannel.value(item.channel);
			foreach(HttpSession *hs, sessions)
			{
				// note: we used to assert that the session was currently a
				//   stream hold and subscribed to the target channel,
				//   however with the new grip-link stuff it is possible for
				//   the session to temporarily switch to NoHold, and for
				//   channels to become unsubscribed. so we'll do a
				//   conditional statement instead
				if(!hs->channels().contains(item.channel))
					continue;

				if(!applyFilters(hs->meta(), item.meta, hs->channels()[item.channel].filters))
					continue;

				streamSessions += hs;

				if(!hs->sid().isEmpty())
					sids += hs->sid();
			}

			if(!item.id.isNull())
				cs.streamLastIds.set(item.channel, item.id);
		}

		if(item.formats.contains(PublishFormat::WebSocketMessage))
		{
			QSet<WsSession*> wsbc = cs.wsSessionsByChannel.value(item.channel);
			foreach(WsSession *s, wsbc)
			{
				assert(s->channels.contains(item.channel));

				if(!applyFilters(s->meta, item.meta, s->channelFilters[item.channel]))
					continue;

				wsSessions += s;

				if(!s->sid.isEmpty())
					sids += s->sid;
			}
		}

		if(!responseSessions.isEmpty())
		{
			PublishItem i = item;
			i.format = item.formats.value(PublishFormat::HttpResponse);
			i.formats.clear();

			PublishFormat &f = i.format;

			QList<QByteArray> exposeHeaders = f.headers.getAll("Grip-Expose-Headers");

			// remove grip headers from the push
			for(int n = 0; n < f.headers.count(); ++n)
			{
				// strip out grip headers
				if(qstrnicmp(f.headers[n].first.data(), "Grip-", 5) == 0)
				{
					f.headers.removeAt(n);
					--n; // adjust position
				}
			}

			log_debug("relaying to %d http-response subscribers", responseSessions.count());

			foreach(HttpSession *hs, responseSessions)
			{
				QString route = hs->route();

				if(!publishLimiter->addAction(route, new PublishAction(this, hs, i, exposeHeaders)))
				{
					if(!route.isEmpty())
						log_warning("exceeded publish hwm (%d) for route %s, dropping message", config.messageHwm, qPrintable(route));
					else
						log_warning("exceeded publish hwm (%d), dropping message", config.messageHwm);
				}

				stats->addMessageSent(route.toUtf8(), "http-response");
			}

			stats->addMessage(i.channel, i.id, "http-response", responseSessions.count());
		}

		if(!streamSessions.isEmpty())
		{
			PublishItem i = item;
			i.format = item.formats.value(PublishFormat::HttpStream);
			i.formats.clear();

			log_debug("relaying to %d http-stream subscribers", streamSessions.count());

			foreach(HttpSession *hs, streamSessions)
			{
				QString route = hs->route();

				publishLimiter->addAction(route, new PublishAction(this, hs, i));
				stats->addMessageSent(route.toUtf8(), "http-stream");
			}

			stats->addMessage(i.channel, i.id, "http-stream", streamSessions.count());
		}

		if(!wsSessions.isEmpty())
		{
			PublishItem i = item;
			i.format = item.formats.value(PublishFormat::WebSocketMessage);
			i.formats.clear();

			log_debug("relaying to %d ws-message subscribers", wsSessions.count());

			foreach(WsSession *s, wsSessions)
			{
				QString route = s->route;

				publishLimiter->addAction(route, new PublishAction(this, s, i));
				stats->addMessageSent(route.toUtf8(), "ws-message");
			}

			stats->addMessage(i.channel, i.id, "ws-message", wsSessions.count());
		}

		int receivers = responseSessions.count() + streamSessions.count() + wsSessions.count();
		log_info("publish channel=%s receivers=%d", qPrintable(item.channel), receivers);

		if(!item.id.isNull() && !sids.isEmpty() && stateClient)
		{
			// update sessions' last-id
			QHash<QString, LastIds> sidLastIds;
			foreach(const QString &sid, sids)
			{
				LastIds lastIds;
				lastIds[item.channel] = item.id;
				sidLastIds[sid] = lastIds;
			}

			Deferred *d = SessionRequest::updateMany(stateClient, sidLastIds, this);
			connect(d, &Deferred::finished, this, &Private::sessionUpdateMany_finished);
			deferreds += d;
		}
	}

	void writeRetryPacket(const RetryRequestPacket &packet)
	{
		if(!retrySock)
		{
			log_error("retry: can't write, no socket");
			return;
		}

		QVariant vout = packet.toVariant();

		log_debug("OUT retry: %s", qPrintable(TnetString::variantToString(vout, -1)));
		retrySock->write(QList<QByteArray>() << TnetString::fromVariant(vout));
	}

	void writeWsControlItem(const WsControlPacket::Item &item)
	{
		if(!wsControlOutSock)
		{
			log_error("wscontrol: can't write, no socket");
			return;
		}

		WsControlPacket out;
		out.items += item;

		QVariant vout = out.toVariant();

		log_debug("OUT wscontrol: %s", qPrintable(TnetString::variantToString(vout, -1)));
		wsControlOutSock->write(QList<QByteArray>() << TnetString::fromVariant(vout));
	}

	void addSub(const QString &channel)
	{
		if(!cs.subs.contains(channel))
		{
			Subscription *sub = new Subscription(channel);
			connect(sub, &Subscription::subscribed, this, &Private::sub_subscribed);
			cs.subs.insert(channel, sub);
			sub->start();

			if(inSubSock)
			{
				log_debug("SUB socket subscribe: %s", qPrintable(channel));
				inSubSock->subscribe(channel.toUtf8());
			}
		}
	}

	void removeSub(const QString &channel)
	{
		if(cs.subs.contains(channel))
		{
			Subscription *sub = cs.subs[channel];
			cs.subs.remove(channel);
			delete sub;

			if(inSubSock)
			{
				log_debug("SUB socket unsubscribe: %s", qPrintable(channel));
				inSubSock->unsubscribe(channel.toUtf8());
			}
		}
	}

	void httpControlRespond(SimpleHttpRequest *req, int code, const QByteArray &reason, const QString &body, const QByteArray &contentType = QByteArray(), const HttpHeaders &headers = HttpHeaders(), int items = -1)
	{
		HttpHeaders outHeaders = headers;
		if(!contentType.isEmpty())
			outHeaders += HttpHeader("Content-Type", contentType);
		else
			outHeaders += HttpHeader("Content-Type", "text/plain");

		req->respond(code, reason, outHeaders, body.toUtf8());
		connect(req, &SimpleHttpRequest::finished, req, &QObject::deleteLater);

		QString msg = QString("control: %1 %2 code=%3 %4").arg(req->requestMethod(), QString::fromUtf8(req->requestUri()), QString::number(code), QString::number(body.size()));
		if(items > -1)
			msg += QString(" items=%1").arg(items);

		log_info("%s", qPrintable(msg));
	}

	void publishSend(QObject *target, const PublishItem &item, const QList<QByteArray> &exposeHeaders)
	{
		const PublishFormat &f = item.format;

		if(f.type == PublishFormat::HttpResponse || f.type == PublishFormat::HttpStream)
		{
			HttpSession *hs = qobject_cast<HttpSession*>(target);

			hs->publish(item, exposeHeaders);
		}
		else if(f.type == PublishFormat::WebSocketMessage)
		{
			WsSession *s = qobject_cast<WsSession*>(target);

			WsControlPacket::Item i;
			i.cid = s->cid.toUtf8();

			if(f.close)
			{
				i.type = WsControlPacket::Item::Close;
				i.code = f.code;
			}
			else
			{
				i.type = WsControlPacket::Item::Send;

				switch(f.messageType)
				{
					case PublishFormat::Text:   i.contentType = "text"; break;
					case PublishFormat::Binary: i.contentType = "binary"; break;
					case PublishFormat::Ping:   i.contentType = "ping"; break;
					case PublishFormat::Pong:   i.contentType = "pong"; break;
					default: return; // unrecognized type, skip
				}

				i.message = f.body;
			}

			writeWsControlItem(i);
		}
	}

private slots:
	void inspectServer_requestReady()
	{
		if(inspectWorkers.count() >= INSPECT_WORKERS_MAX)
			return;

		ZrpcRequest *req = inspectServer->takeNext();
		if(!req)
			return;

		InspectWorker *w = new InspectWorker(req, stateClient, config.shareAll, this);
		connect(w, &Deferred::finished, this, &Private::inspectWorker_finished);
		inspectWorkers += w;
	}

	void acceptServer_requestReady()
	{
		if(acceptWorkers.count() >= ACCEPT_WORKERS_MAX)
			return;

		ZrpcRequest *req = acceptServer->takeNext();
		if(!req)
			return;

		AcceptWorker *w = new AcceptWorker(req, stateClient, &cs, zhttpIn, zhttpOut, stats, updateLimiter, this);
		connect(w, &AcceptWorker::finished, this, &Private::acceptWorker_finished);
		connect(w, &AcceptWorker::sessionsReady, this, &Private::acceptWorker_sessionsReady);
		connect(w, &AcceptWorker::retryPacketReady, this, &Private::acceptWorker_retryPacketReady);
		acceptWorkers += w;
		w->start();
	}

	void controlServer_requestReady()
	{
		ZrpcRequest *req = controlServer->takeNext();
		if(!req)
			return;

		log_debug("IN command: %s args=%s", qPrintable(req->method()), qPrintable(TnetString::variantToString(req->args(), -1)));

		if(req->method() == "conncheck")
		{
			new ConnCheckWorker(req, proxyControlClient, stats, this);
		}
		else if(req->method() == "get-zmq-uris")
		{
			QVariantHash out;
			if(!config.commandSpec.isEmpty())
				out["command"] = config.commandSpec.toUtf8();
			if(!config.pushInSpec.isEmpty())
				out["publish-pull"] = config.pushInSpec.toUtf8();
			if(!config.pushInSubSpec.isEmpty())
				out["publish-sub"] = config.pushInSubSpec.toUtf8();
			req->respond(out);
			delete req;
		}
		else
		{
			req->respondError("method-not-found");
			delete req;
		}
	}

	void inPull_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("IN pull: received message with parts != 1, skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(message[0], 0, &ok);
		if(!ok)
		{
			log_warning("IN pull: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		log_debug("IN pull: %s", qPrintable(TnetString::variantToString(data, -1)));

		QString errorMessage;
		PublishItem item = PublishItem::fromVariant(data, QString(), &ok, &errorMessage);
		if(!ok)
		{
			log_warning("IN pull: received message with invalid format: %s, skipping", qPrintable(errorMessage));
			return;
		}

		handlePublishItem(item);
	}

	void inSub_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 2)
		{
			log_warning("IN sub: received message with parts != 2, skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(message[1], 0, &ok);
		if(!ok)
		{
			log_warning("IN sub: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		QString channel = QString::fromUtf8(message[0]);

		log_debug("IN sub: channel=%s %s", qPrintable(channel), qPrintable(TnetString::variantToString(data, -1)));

		QString errorMessage;
		PublishItem item = PublishItem::fromVariant(data, channel, &ok, &errorMessage);
		if(!ok)
		{
			log_warning("IN sub: received message with invalid format: %s, skipping", qPrintable(errorMessage));
			return;
		}

		handlePublishItem(item);
	}

	void wsControlIn_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("IN wscontrol: received message with parts != 1, skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(message[0], 0, &ok);
		if(!ok)
		{
			log_warning("IN wscontrol: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		log_debug("IN wscontrol: %s", qPrintable(TnetString::variantToString(data, -1)));

		WsControlPacket packet;
		if(!packet.fromVariant(data))
		{
			log_warning("IN wscontrol: received message with invalid format, skipping");
			return;
		}

		QStringList updateSids;

		foreach(const WsControlPacket::Item &item, packet.items)
		{
			if(item.type == WsControlPacket::Item::Here)
			{
				WsSession *s = cs.wsSessions.value(item.cid);
				if(!s)
				{
					s = new WsSession(this);
					connect(s, &WsSession::expired, this, &Private::wssession_expired);
					s->cid = QString::fromUtf8(item.cid);
					s->ttl = item.ttl;
					s->requestData.uri = item.uri;
					s->refreshExpiration();
					cs.wsSessions.insert(s->cid, s);
					log_debug("added ws session: %s", qPrintable(s->cid));
				}

				s->route = item.route;
				s->channelPrefix = QString::fromUtf8(item.channelPrefix);
				continue;
			}

			// any other type must be for a known cid
			WsSession *s = cs.wsSessions.value(QString::fromUtf8(item.cid));
			if(!s)
			{
				// send cancel, causing the proxy to close the connection. client
				//   will need to retry to repair
				WsControlPacket::Item i;
				i.cid = item.cid;
				i.type = WsControlPacket::Item::Cancel;
				writeWsControlItem(i);
				continue;
			}

			if(item.type == WsControlPacket::Item::KeepAlive)
			{
				s->ttl = item.ttl;
				s->refreshExpiration();
			}
			else if(item.type == WsControlPacket::Item::Gone || item.type == WsControlPacket::Item::Cancel)
			{
				QSet<QString> unsubs = cs.removeWsSessionChannels(s);

				foreach(const QString &channel, unsubs)
					stats->removeSubscription("ws", channel, false);

				log_debug("removed ws session: %s", qPrintable(s->cid));

				cs.wsSessions.remove(s->cid);
				delete s;
			}
			else if(item.type == WsControlPacket::Item::Grip)
			{
				QJsonParseError e;
				QJsonDocument doc = QJsonDocument::fromJson(item.message, &e);
				if(e.error != QJsonParseError::NoError || (!doc.isObject() && !doc.isArray()))
				{
					log_debug("grip control message is not valid json");
					return;
				}

				if(doc.isObject())
					data = doc.object().toVariantMap();
				else // isArray
					data = doc.array().toVariantList();

				QString errorMessage;
				WsControlMessage cm = WsControlMessage::fromVariant(data, &ok, &errorMessage);
				if(!ok)
				{
					log_debug("failed to parse grip control message: %s", qPrintable(errorMessage));
					return;
				}

				if(cm.type == WsControlMessage::Subscribe)
				{
					QString channel = s->channelPrefix + cm.channel;
					s->channels += channel;
					s->channelFilters[channel] = cm.filters;

					if(!cs.wsSessionsByChannel.contains(channel))
						cs.wsSessionsByChannel.insert(channel, QSet<WsSession*>());

					cs.wsSessionsByChannel[channel] += s;

					log_debug("ws session %s subscribed to %s", qPrintable(s->cid), qPrintable(channel));

					stats->addSubscription("ws", channel);
					addSub(channel);

					log_info("subscribe %s channel=%s", qPrintable(s->requestData.uri.toString(QUrl::FullyEncoded)), qPrintable(channel));
				}
				else if(cm.type == WsControlMessage::Unsubscribe)
				{
					QString channel = s->channelPrefix + cm.channel;
					s->channels.remove(channel);
					s->channelFilters.remove(channel);

					if(cs.wsSessionsByChannel.contains(channel))
					{
						QSet<WsSession*> &cur = cs.wsSessionsByChannel[channel];
						cur.remove(s);
						if(cur.isEmpty())
						{
							cs.wsSessionsByChannel.remove(channel);
							stats->removeSubscription("ws", channel, false);
						}
					}
				}
				else if(cm.type == WsControlMessage::Detach)
				{
					WsControlPacket::Item i;
					i.cid = item.cid;
					i.type = WsControlPacket::Item::Detach;
					writeWsControlItem(i);
				}
				else if(cm.type == WsControlMessage::Session)
				{
					s->sid = cm.sessionId;
					updateSids += cm.sessionId;
				}
				else if(cm.type == WsControlMessage::SetMeta)
				{
					if(!cm.metaValue.isNull())
						s->meta[cm.metaName] = cm.metaValue;
					else
						s->meta.remove(cm.metaName);
				}
				else if(cm.type == WsControlMessage::KeepAlive)
				{
					WsControlPacket::Item i;
					i.cid = item.cid;
					i.type = WsControlPacket::Item::KeepAliveSetup;

					if(!cm.content.isNull())
					{
						QString contentType;
						switch(cm.messageType)
						{
							case WsControlMessage::Text:   contentType = "text"; break;
							case WsControlMessage::Binary: contentType = "binary"; break;
							case WsControlMessage::Ping:   contentType = "ping"; break;
							case WsControlMessage::Pong:   contentType = "pong"; break;
							default: continue; // unrecognized type, ignore
						}

						s->keepAliveType = contentType.toUtf8();
						s->keepAliveMessage = cm.content;

						i.timeout = (cm.timeout > 0 ? cm.timeout : DEFAULT_WS_KEEPALIVE_TIMEOUT);
					}
					else
					{
						s->keepAliveType.clear();
						s->keepAliveMessage.clear();
					}

					writeWsControlItem(i);
				}
			}
			else if(item.type == WsControlPacket::Item::NeedKeepAlive)
			{
				if(!s->keepAliveMessage.isNull())
				{
					WsControlPacket::Item i;
					i.cid = s->cid.toUtf8();
					i.type = WsControlPacket::Item::Send;
					i.contentType = s->keepAliveType;
					i.message = s->keepAliveMessage;

					writeWsControlItem(i);

					stats->addActivity(s->route.toUtf8(), 1);
				}
			}
		}

		if(stateClient && !updateSids.isEmpty())
		{
			foreach(const QString &sid, updateSids)
			{
				Deferred *d = SessionRequest::createOrUpdate(stateClient, sid, LastIds(), this);
				connect(d, &Deferred::finished, this, &Private::sessionCreateOrUpdate_finished);
				deferreds += d;
			}
		}
	}

	void proxyStats_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("IN proxy stats: received message with parts != 1, skipping");
			return;
		}

		int at = message[0].indexOf(' ');
		if(at == -1)
		{
			log_warning("IN proxy stats: received message with invalid format, skipping");
			return;
		}

		QByteArray type = message[0].mid(0, at);

		if(at + 1 >= message[0].length() || message[0][at + 1] != 'T')
		{
			log_warning("IN proxy stats: received message with unsupported format, skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(message[0], at + 2, &ok);
		if(!ok)
		{
			log_warning("IN proxy stats: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		log_debug("IN proxy stats: %s %s", type.data(), qPrintable(TnetString::variantToString(data, -1)));

		StatsPacket p;
		if(!p.fromVariant(type, data))
		{
			log_warning("IN proxy stats: received message with invalid format, skipping");
			return;
		}

		if(p.type == StatsPacket::Activity)
		{
			if(p.count > 0)
			{
				// merge with our own stats
				stats->addActivity(p.route, p.count);
			}
		}
		else if(p.type == StatsPacket::Connected || p.type == StatsPacket::Disconnected)
		{
			QString sid;
			if(p.connectionType == StatsPacket::WebSocket)
			{
				WsSession *s = cs.wsSessions.value(QString::fromUtf8(p.connectionId));
				if(s)
					sid = s->sid;
			}

			// track proxy connections for reporting
			stats->processExternalPacket(p);

			// forward the packet. this will stamp the from field and keep the rest
			stats->sendPacket(p);

			// update session
			if(stateClient && !sid.isEmpty() && p.type == StatsPacket::Connected)
			{
				QHash<QString, LastIds> sidLastIds;
				sidLastIds[sid] = LastIds();
				Deferred *d = SessionRequest::updateMany(stateClient, sidLastIds, this);
				connect(d, &Deferred::finished, this, &Private::sessionUpdateMany_finished);
				deferreds += d;
				return;
			}
		}
	}

	void controlHttpServer_requestReady()
	{
		SimpleHttpRequest *req = controlHttpServer->takeNext();
		if(!req)
			return;

		QByteArray path = req->requestUri();
		if(path.endsWith("/"))
			path.truncate(path.length() - 1);

		HttpHeaders headers = req->requestHeaders();

		QByteArray responseContentType;
		if(headers.contains("Accept"))
		{
			foreach(const HttpHeaderParameters &params, headers.getAllAsParameters("Accept"))
			{
				if(params.isEmpty() || params[0].first.isEmpty())
					continue;

				QList<QByteArray> type = params[0].first.split('/');
				if(type[0] == "text" && type[1] == "plain")
				{
					responseContentType = "text/plain";
					break;
				}
				else if(type[0] == "application" && type[1] == "json")
				{
					responseContentType = "application/json";
					break;
				}
				else if(type[0] == "text" && type[1] == "*")
				{
					responseContentType = "text/plain";
					break;
				}
				else if(type[0] == "application" && type[1] == "*")
				{
					responseContentType = "application/json";
					break;
				}
				else if(type[0] == "*" && type[1] == "*")
				{
					responseContentType = "text/plain";
					break;
				}
			}

			if(responseContentType.isEmpty())
			{
				httpControlRespond(req, 406, "Not Acceptable", "Not Acceptable. Supported formats are text/plain and application/json.\n");
				return;
			}
		}
		else
		{
			responseContentType = "text/plain";
		}

		if(path == "/publish")
		{
			if(req->requestMethod() == "POST")
			{
				QJsonParseError e;
				QJsonDocument doc = QJsonDocument::fromJson(req->requestBody(), &e);
				if(e.error != QJsonParseError::NoError)
				{
					httpControlRespond(req, 400, "Bad Request", "Body is not valid JSON.\n");
					return;
				}

				if(!doc.isObject())
				{
					httpControlRespond(req, 400, "Bad Request", "Invalid format.\n");
					return;
				}

				QVariantMap mdata = doc.object().toVariantMap();
				QVariantList vitems;

				if(!mdata.contains("items"))
				{
					httpControlRespond(req, 400, "Bad Request", "Invalid format: object does not contain 'items'\n");
					return;
				}

				if(mdata["items"].type() != QVariant::List)
				{
					httpControlRespond(req, 400, "Bad Request", "Invalid format: object contains 'items' with wrong type\n");
					return;
				}

				vitems = mdata["items"].toList();

				bool ok;
				QString errorMessage;
				QList<PublishItem> items = parseHttpItems(vitems, &ok, &errorMessage);
				if(!ok)
				{
					httpControlRespond(req, 400, "Bad Request", QString("Invalid format: %1\n").arg(errorMessage));
					return;
				}

				QString message = "Published";
				if(responseContentType == "application/json")
				{
					QVariantMap obj;
					obj["message"] = message;
					QString body = QJsonDocument(QJsonObject::fromVariantMap(obj)).toJson(QJsonDocument::Compact);
					httpControlRespond(req, 200, "OK", body + "\n", responseContentType, HttpHeaders(), items.count());
				}
				else // text/plain
				{
					httpControlRespond(req, 200, "OK", message + "\n", responseContentType, HttpHeaders(), items.count());
				}

				foreach(const PublishItem &item, items)
					handlePublishItem(item);
			}
			else
			{
				HttpHeaders headers;
				headers += HttpHeader("Allow", "POST");
				httpControlRespond(req, 405, "Method Not Allowed", "Method not allowed: " + req->requestMethod() + ".\n", QByteArray(), headers);
			}
		}
		else
		{
			httpControlRespond(req, 404, "Not Found", "Not Found\n");
		}
	}

	void sessionCreateOrUpdate_finished(const DeferredResult &result)
	{
		Deferred *d = (Deferred *)sender();
		deferreds.remove(d);

		if(!result.success)
			log_error("couldn't create/update session: condition=%d", result.value.toInt());
	}

	void sessionUpdateMany_finished(const DeferredResult &result)
	{
		Deferred *d = (Deferred *)sender();
		deferreds.remove(d);

		if(!result.success)
			log_error("couldn't update session: condition=%d", result.value.toInt());
	}

	void inspectWorker_finished(const DeferredResult &result)
	{
		Q_UNUSED(result);

		InspectWorker *w = (InspectWorker *)sender();
		inspectWorkers.remove(w);

		// try to read again
		inspectServer_requestReady();
	}

	void acceptWorker_finished(const DeferredResult &result)
	{
		Q_UNUSED(result);

		AcceptWorker *w = (AcceptWorker *)sender();
		acceptWorkers.remove(w);

		// try to read again
		acceptServer_requestReady();
	}

	void acceptWorker_sessionsReady()
	{
		AcceptWorker *w = (AcceptWorker *)sender();

		QList<HttpSession*> sessions = w->takeSessions();
		foreach(HttpSession *hs, sessions)
		{
			hs->setParent(this);
			connect(hs, &HttpSession::subscribe, this, &Private::hs_subscribe);
			connect(hs, &HttpSession::unsubscribe, this, &Private::hs_unsubscribe);
			connect(hs, &HttpSession::finished, this, &Private::hs_finished);
			cs.httpSessions.insert(hs->rid(), hs);

			hs->start();
		}
	}

	void acceptWorker_retryPacketReady(const RetryRequestPacket &packet)
	{
		writeRetryPacket(packet);
	}

	void hs_subscribe(const QString &channel)
	{
		HttpSession *hs = (HttpSession *)sender();

		Instruct::HoldMode mode = hs->holdMode();
		assert(mode == Instruct::ResponseHold || mode == Instruct::StreamHold);

		QHash<QString, QSet<HttpSession*> > *sessionsByChannel;

		if(mode == Instruct::ResponseHold)
		{
			log_debug("adding response hold on %s", qPrintable(channel));

			sessionsByChannel = &cs.responseSessionsByChannel;
		}
		else // StreamHold
		{
			log_debug("adding stream hold on %s", qPrintable(channel));

			sessionsByChannel = &cs.streamSessionsByChannel;
		}

		if(!sessionsByChannel->contains(channel))
			sessionsByChannel->insert(channel, QSet<HttpSession*>());

		(*sessionsByChannel)[channel] += hs;

		log_info("subscribe %s channel=%s", qPrintable(hs->requestUri().toString(QUrl::FullyEncoded)), qPrintable(channel));

		stats->addSubscription(mode == Instruct::ResponseHold ? "response" : "stream", channel);
		addSub(channel);
	}

	void hs_unsubscribe(const QString &channel)
	{
		HttpSession *hs = (HttpSession *)sender();

		Instruct::HoldMode mode = hs->holdMode();
		assert(mode == Instruct::ResponseHold || mode == Instruct::StreamHold);

		QHash<QString, QSet<HttpSession*> > *sessionsByChannel;

		if(mode == Instruct::ResponseHold)
		{
			sessionsByChannel = &cs.responseSessionsByChannel;
		}
		else // StreamHold
		{
			sessionsByChannel = &cs.streamSessionsByChannel;
		}

		if(sessionsByChannel->contains(channel))
		{
			QSet<HttpSession*> &cur = (*sessionsByChannel)[channel];
			if(cur.contains(hs))
			{
				cur.remove(hs);

				if(cur.isEmpty())
				{
					sessionsByChannel->remove(channel);

					if(mode == Instruct::ResponseHold)
					{
						// linger the unsub in case client long-polls again
						stats->removeSubscription("response", channel, true);
					}
					else // StreamHold
					{
						stats->removeSubscription("stream", channel, false);
					}
				}
			}
		}
	}

	void hs_finished()
	{
		HttpSession *hs = (HttpSession *)sender();

		cs.httpSessions.remove(hs->rid());
		delete hs;
	}

	void wssession_expired()
	{
		WsSession *s = (WsSession *)sender();

		QSet<QString> unsubs = cs.removeWsSessionChannels(s);

		foreach(const QString &channel, unsubs)
			stats->removeSubscription("ws", channel, false);

		cs.wsSessions.remove(s->cid);
		delete s;
	}

	void sub_subscribed()
	{
		Subscription *sub = (Subscription *)sender();

		QSet<HttpSession*> sessions = cs.streamSessionsByChannel.value(sub->channel());
		foreach(HttpSession *hs, sessions)
			hs->update();
	}

	void stats_connectionsRefreshed(const QList<QByteArray> &ids)
	{
		if(stateClient)
		{
			// find sids of the connections
			QHash<QString, LastIds> sidLastIds;
			foreach(const QByteArray &id, ids)
			{
				int at = id.indexOf(':');
				assert(at != -1);
				ZhttpRequest::Rid rid(id.mid(0, at), id.mid(at + 1));

				HttpSession *hs = cs.httpSessions.value(rid);
				if(hs && !hs->sid().isEmpty())
					sidLastIds[hs->sid()] = LastIds();
			}

			if(!sidLastIds.isEmpty())
			{
				Deferred *d = SessionRequest::updateMany(stateClient, sidLastIds, this);
				connect(d, &Deferred::finished, this, &Private::sessionUpdateMany_finished);
				deferreds += d;
			}
		}
	}

	void stats_unsubscribed(const QString &mode, const QString &channel)
	{
		// NOTE: this callback may be invoked while looping over certain structures,
		//   so be careful what you touch

		Q_UNUSED(mode);

		if(!cs.responseSessionsByChannel.contains(channel) && !cs.streamSessionsByChannel.contains(channel) && !cs.wsSessionsByChannel.contains(channel))
			removeSub(channel);
	}

	void stats_reported(const QList<StatsPacket> &packets)
	{
		// only one outstanding report at a time
		if(report)
			return;

		// consolidate data
		StatsPacket all;
		all.connectionsMax = 0;
		all.connectionsMinutes = 0;
		all.messagesReceived = 0;
		all.messagesSent = 0;
		all.httpResponseMessagesSent = 0;
		foreach(const StatsPacket &p, packets)
		{
			all.connectionsMax += p.connectionsMax;
			all.connectionsMinutes += p.connectionsMinutes;
			all.messagesReceived += p.messagesReceived;
			all.messagesSent += p.messagesSent;
			all.httpResponseMessagesSent += p.httpResponseMessagesSent;
		}

		report = ControlRequest::report(proxyControlClient, all, this);
		connect(report, &Deferred::finished, this, &Private::report_finished);
		deferreds += report;
	}

	void report_finished(const DeferredResult &result)
	{
		Q_UNUSED(result);

		deferreds.remove(report);
		report = 0;
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
