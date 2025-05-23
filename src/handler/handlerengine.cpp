/*
 * Copyright (C) 2015-2023 Fanout, Inc.
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

#include "handlerengine.h"

#include <assert.h>
#include <algorithm>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "qzmqreqmessage.h"
#include "qtcompat.h"
#include "tnetstring.h"
#include "timer.h"
#include "defercall.h"
#include "log.h"
#include "logutil.h"
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
#include "wssession.h"
#include "controlrequest.h"
#include "conncheckworker.h"
#include "refreshworker.h"
#include "ratelimiter.h"
#include "httpsessionupdatemanager.h"
#include "sequencer.h"
#include "filterstack.h"

#define DEFAULT_HWM 101000
#define SUB_SNDHWM 0 // infinite
#define RETRY_WAIT_TIME 0
#define WSCONTROL_WAIT_TIME 0
#define STATE_RPC_TIMEOUT 1000
#define PROXY_RPC_TIMEOUT 10000
#define DEFAULT_WS_KEEPALIVE_TIMEOUT 55
#define DEFAULT_WS_SENDDELAYED_TIMEOUT 1
#define SUBSCRIBED_DELAY 1000

#define INSPECT_WORKERS_MAX 10
#define ACCEPT_WORKERS_MAX 10

using namespace VariantUtil;

static QList<PublishItem> parseItems(const QVariantList &vitems, bool *ok = 0, QString *errorMessage = 0)
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
public:
	std::unique_ptr<ZrpcRequest> req;
	ZrpcManager *stateClient;
	bool shareAll;
	HttpRequestData requestData;
	bool truncated;
	bool autoShare;
	QString sid;
	LastIds lastIds;
	std::map<Deferred*, std::unique_ptr<Deferred>> deferreds;

	InspectWorker(ZrpcRequest *_req, ZrpcManager *_stateClient, bool _shareAll) :
		req(_req),
		stateClient(_stateClient),
		shareAll(_shareAll),
		truncated(false),
		autoShare(false)
	{
		if(req->method() == "inspect")
		{
			QVariantHash args = req->args();

			if(!args.contains("method") || typeId(args["method"]) != QMetaType::QByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.method = QString::fromLatin1(args["method"].toByteArray());

			if(!args.contains("uri") || typeId(args["uri"]) != QMetaType::QByteArray)
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

			if(!args.contains("headers") || typeId(args["headers"]) != QMetaType::QVariantList)
			{
				respondError("bad-request");
				return;
			}

			foreach(const QVariant &vheader, args["headers"].toList())
			{
				if(typeId(vheader) != QMetaType::QVariantList)
				{
					respondError("bad-request");
					return;
				}

				QVariantList vlist = vheader.toList();
				if(vlist.count() != 2 || typeId(vlist[0]) != QMetaType::QByteArray || typeId(vlist[1]) != QMetaType::QByteArray)
				{
					respondError("bad-request");
					return;
				}

				requestData.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
			}

			if(!args.contains("body") || typeId(args["body"]) != QMetaType::QByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.body = args["body"].toByteArray();

			truncated = false;
			if(args.contains("truncated"))
			{
				if(typeId(args["truncated"]) != QMetaType::Bool)
				{
					respondError("bad-request");
					return;
				}

				truncated = args["truncated"].toBool();
			}

			bool getSession = false;
			if(args.contains("get-session"))
			{
				if(typeId(args["get-session"]) != QMetaType::Bool)
				{
					respondError("bad-request");
					return;
				}

				getSession = args["get-session"].toBool();
			}

			autoShare = false;
			if(args.contains("auto-share"))
			{
				if(typeId(args["auto-share"]) != QMetaType::Bool)
				{
					respondError("bad-request");
					return;
				}

				autoShare = args["auto-share"].toBool();
			}

			if(getSession && stateClient)
			{
				// determine session info

				auto d = std::unique_ptr<Deferred>(SessionRequest::detectRulesGet(stateClient, requestData.uri.host().toUtf8(), requestData.uri.path(QUrl::FullyEncoded).toUtf8()));

				// safe to not track, since d can't outlive this
				d->finished.connect(boost::bind(&InspectWorker::sessionDetectRulesGet_finished, this, d.get(), boost::placeholders::_1));

				deferreds[d.get()] = std::move(d);
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

	void sessionDetectRulesGet_finished(Deferred *d, const DeferredResult &result)
	{
		deferreds.erase(d);

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
					jsonData = tmp.queryItemValue(rule.jsonParam, QUrl::FullyDecoded).toUtf8();
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
				auto d = std::unique_ptr<Deferred>(SessionRequest::getLastIds(stateClient, sid));

				// safe to not track, since d can't outlive this
				d->finished.connect(boost::bind(&InspectWorker::sessionGetLastIds_finished, this, d.get(), boost::placeholders::_1));

				deferreds[d.get()] = std::move(d);
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

	void sessionGetLastIds_finished(Deferred *d, const DeferredResult &result)
	{
		deferreds.erase(d);

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

class Subscription;

class CommonState
{
public:
	QHash<ZhttpRequest::Rid, std::shared_ptr<HttpSession>> httpSessions;
	QHash<QString, std::shared_ptr<WsSession>> wsSessions;
	QHash<QString, QSet<HttpSession*> > responseSessionsByChannel;
	QHash<QString, QSet<HttpSession*> > streamSessionsByChannel;
	QHash<QString, QSet<WsSession*> > wsSessionsByChannel;
	PublishLastIds publishLastIds;
	QHash<QString, Subscription*> subs;

	CommonState() :
		publishLastIds(1000000)
	{
	}
};

class AcceptWorker : public Deferred
{
public:
	std::unique_ptr<ZrpcRequest> req;
	ZrpcManager *stateClient;
	CommonState *cs;
	ZhttpManager *zhttpIn;
	ZhttpManager *zhttpOut;
	StatsManager *stats;
	RateLimiter *updateLimiter;
	std::shared_ptr<RateLimiter> filterLimiter;
	std::shared_ptr<HttpSessionUpdateManager> httpSessionUpdateManager;
	QString route;
	QString statsRoute;
	QString channelPrefix;
	int logLevel;
	QStringList implicitChannels;
	bool trusted;
	QHash<ZhttpRequest::Rid, RequestState> requestStates;
	HttpRequestData requestData;
	HttpRequestData origRequestData;
	bool haveInspectInfo;
	InspectData inspectInfo;
	HttpResponseData responseData;
	bool responseSent;
	QString sid;
	LastIds lastIds;
	QList<std::shared_ptr<HttpSession>> sessions;
	int connectionSubscriptionMax;
	QSet<QByteArray> needRemoveFromStats;
	std::map<Deferred*, std::unique_ptr<Deferred>> deferreds;

	AcceptWorker(ZrpcRequest *_req, ZrpcManager *_stateClient, CommonState *_cs, ZhttpManager *_zhttpIn, ZhttpManager *_zhttpOut, StatsManager *_stats, RateLimiter *_updateLimiter, const std::shared_ptr<RateLimiter> &_filterLimiter, const std::shared_ptr<HttpSessionUpdateManager> &_httpSessionUpdateManager, int _connectionSubscriptionMax) :
		req(_req),
		stateClient(_stateClient),
		cs(_cs),
		zhttpIn(_zhttpIn),
		zhttpOut(_zhttpOut),
		stats(_stats),
		updateLimiter(_updateLimiter),
		filterLimiter(_filterLimiter),
		httpSessionUpdateManager(_httpSessionUpdateManager),
		logLevel(-1),
		trusted(false),
		haveInspectInfo(false),
		responseSent(false),
		connectionSubscriptionMax(_connectionSubscriptionMax)
	{
	}

	~AcceptWorker()
	{
		foreach(const QByteArray &cid, needRemoveFromStats)
			stats->removeConnection(cid, false);
	}

	// NOTE: to ensure sequential processing of conn-max packets, this
	// method must process any such packets contained within the accept
	// request before returning. the conn-max packets must not be processed
	// asynchronously
	void start()
	{
		QVariantHash args = req->args();

		// process conn-max packets before doing anything else
		if(args.contains("conn-max"))
		{
			if(typeId(args["conn-max"]) != QMetaType::QVariantList)
			{
				respondError("bad-request");
				return;
			}

			QVariantList packets = args["conn-max"].toList();

			foreach(const QVariant &data, packets)
			{
				StatsPacket p;
				if(!p.fromVariant("conn-max", data) || p.type != StatsPacket::ConnectionsMax)
				{
					respondError("bad-request");
					return;
				}

				stats->processExternalPacket(p, false);
			}
		}

		if(args.contains("route"))
		{
			if(typeId(args["route"]) != QMetaType::QByteArray)
			{
				respondError("bad-request");
				return;
			}

			route = QString::fromUtf8(args["route"].toByteArray());
		}

		if(args.contains("separate-stats"))
		{
			if(typeId(args["separate-stats"]) != QMetaType::Bool)
			{
				respondError("bad-request");
				return;
			}

			bool separateStats = args["separate-stats"].toBool();

			if(!route.isEmpty() && separateStats)
				statsRoute = route;
		}

		if(args.contains("channel-prefix"))
		{
			if(typeId(args["channel-prefix"]) != QMetaType::QByteArray)
			{
				respondError("bad-request");
				return;
			}

			channelPrefix = QString::fromUtf8(args["channel-prefix"].toByteArray());
		}

		if(args.contains("log-level"))
		{
			if(!canConvert(args["log-level"], QMetaType::Int))
			{
				respondError("bad-request");
				return;
			}

			logLevel = args["log-level"].toInt();
		}

		if(args.contains("channels"))
		{
			if(typeId(args["channels"]) != QMetaType::QVariantList)
			{
				respondError("bad-request");
				return;
			}

			QVariantList vchannels = args["channels"].toList();
			foreach(const QVariant &v, vchannels)
			{
				if(typeId(v) != QMetaType::QByteArray)
				{
					respondError("bad-request");
					return;
				}

				implicitChannels += QString::fromUtf8(v.toByteArray());
			}
		}

		if(args.contains("trusted"))
		{
			if(typeId(args["trusted"]) != QMetaType::Bool)
			{
				respondError("bad-request");
				return;
			}

			trusted = args["trusted"].toBool();
		}

		// parse requests

		if(!args.contains("requests") || typeId(args["requests"]) != QMetaType::QVariantList)
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

		requestData = parseRequestData(args, "request-data");
		if(requestData.method.isEmpty())
		{
			respondError("bad-request");
			return;
		}

		// parse orig-request-data

		origRequestData = parseRequestData(args, "orig-request-data");
		if(origRequestData.method.isEmpty())
		{
			respondError("bad-request");
			return;
		}

		// parse response

		if(!args.contains("response") || typeId(args["response"]) != QMetaType::QVariantHash)
		{
			respondError("bad-request");
			return;
		}

		QVariantHash rd = args["response"].toHash();

		if(!rd.contains("code") || !canConvert(rd["code"], QMetaType::Int))
		{
			respondError("bad-request");
			return;
		}

		responseData.code = rd["code"].toInt();

		if(!rd.contains("reason") || typeId(rd["reason"]) != QMetaType::QByteArray)
		{
			respondError("bad-request");
			return;
		}

		responseData.reason = rd["reason"].toByteArray();

		if(!rd.contains("headers") || typeId(rd["headers"]) != QMetaType::QVariantList)
		{
			respondError("bad-request");
			return;
		}

		foreach(const QVariant &vheader, rd["headers"].toList())
		{
			if(typeId(vheader) != QMetaType::QVariantList)
			{
				respondError("bad-request");
				return;
			}

			QVariantList vlist = vheader.toList();
			if(vlist.count() != 2 || typeId(vlist[0]) != QMetaType::QByteArray || typeId(vlist[1]) != QMetaType::QByteArray)
			{
				respondError("bad-request");
				return;
			}

			responseData.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
		}

		if(!rd.contains("body") || typeId(rd["body"]) != QMetaType::QByteArray)
		{
			respondError("bad-request");
			return;
		}

		responseData.body = rd["body"].toByteArray();

		if(args.contains("inspect"))
		{
			if(typeId(args["inspect"]) != QMetaType::QVariantHash)
			{
				respondError("bad-request");
				return;
			}

			QVariantHash vinspect = args["inspect"].toHash();

			if(!vinspect.contains("no-proxy") || typeId(vinspect["no-proxy"]) != QMetaType::Bool)
			{
				respondError("bad-request");
				return;
			}

			inspectInfo.doProxy = !vinspect["no-proxy"].toBool();

			inspectInfo.sharingKey.clear();
			if(vinspect.contains("sharing-key"))
			{
				if(typeId(vinspect["sharing-key"]) != QMetaType::QByteArray)
				{
					respondError("bad-request");
					return;
				}

				inspectInfo.sharingKey = vinspect["sharing-key"].toByteArray();
			}

			if(vinspect.contains("sid"))
			{
				if(typeId(vinspect["sid"]) != QMetaType::QByteArray)
				{
					respondError("bad-request");
					return;
				}

				inspectInfo.sid = vinspect["sid"].toByteArray();
			}

			if(vinspect.contains("last-ids"))
			{
				if(typeId(vinspect["last-ids"]) != QMetaType::QVariantHash)
				{
					respondError("bad-request");
					return;
				}

				QVariantHash vlastIds = vinspect["last-ids"].toHash();
				QHashIterator<QString, QVariant> it(vlastIds);
				while(it.hasNext())
				{
					it.next();

					if(typeId(it.value()) != QMetaType::QByteArray)
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
			if(typeId(args["response-sent"]) != QMetaType::Bool)
			{
				respondError("bad-request");
				return;
			}

			responseSent = args["response-sent"].toBool();
		}

		bool useSession = false;
		if(args.contains("use-session"))
		{
			if(typeId(args["use-session"]) != QMetaType::Bool)
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

		// we need to "atomically" process conn-max packets and add
		// connections to the stats manager. we do this by processing the
		// conn-max packets above and adding to the stats manager below,
		// without returning to the event loop in between
		foreach(const RequestState &rs, requestStates)
		{
			QByteArray cid = rs.rid.first + ':' + rs.rid.second;

			int reportOffset = stats->connectionSendEnabled() ? -1 : qMax(rs.unreportedTime, 0);

			needRemoveFromStats += cid;
			stats->addConnection(cid, statsRoute.toUtf8(), StatsManager::Http, rs.logicalPeerAddress, rs.isHttps, true, reportOffset);
		}

		if(useSession && stateClient)
		{
			if(!rules.isEmpty())
			{
				auto d = std::unique_ptr<Deferred>(SessionRequest::detectRulesSet(stateClient, rules));

				// safe to not track, since d can't outlive this
				d->finished.connect(boost::bind(&AcceptWorker::sessionDetectRulesSet_finished, this, d.get(), boost::placeholders::_1));

				deferreds[d.get()] = std::move(d);
			}
			else
			{
				afterSetRules();
			}

			return;
		}

		afterSessionCalls();
	}

	QList<std::shared_ptr<HttpSession>> takeSessions()
	{
		// swap instead of std::move since sessions is a member and should have a known state
		QList<std::shared_ptr<HttpSession>> out;
		out.swap(sessions);
		return out;
	}

	Signal sessionsReady;
	boost::signals2::signal<void(const QByteArray &,const RetryRequestPacket&)> retryPacketReady;

private:
	static HttpRequestData parseRequestData(const QVariantHash &args, const QString &field)
	{
		if(!args.contains(field) || typeId(args[field]) != QMetaType::QVariantHash)
			return HttpRequestData();

		QVariantHash rd = args[field].toHash();

		if(!rd.contains("method") || typeId(rd["method"]) != QMetaType::QByteArray)
			return HttpRequestData();

		HttpRequestData out;
		out.method = QString::fromLatin1(rd["method"].toByteArray());

		if(!rd.contains("uri") || typeId(rd["uri"]) != QMetaType::QByteArray)
			return HttpRequestData();

		out.uri = QUrl(rd["uri"].toString(), QUrl::StrictMode);
		if(!out.uri.isValid())
			return HttpRequestData();

		if(!rd.contains("headers") || typeId(rd["headers"]) != QMetaType::QVariantList)
			return HttpRequestData();

		foreach(const QVariant &vheader, rd["headers"].toList())
		{
			if(typeId(vheader) != QMetaType::QVariantList)
				return HttpRequestData();

			QVariantList vlist = vheader.toList();
			if(vlist.count() != 2 || typeId(vlist[0]) != QMetaType::QByteArray || typeId(vlist[1]) != QMetaType::QByteArray)
				return HttpRequestData();

			out.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
		}

		if(!rd.contains("body") || typeId(rd["body"]) != QMetaType::QByteArray)
			return HttpRequestData();

		out.body = rd["body"].toByteArray();

		return out;
	}

	void respondError(const QByteArray &condition, const QVariant &result = QVariant())
	{
		req->respondError(condition, result);
		setFinished(true);
	}

	void afterSetRules()
	{
		if(!sid.isEmpty())
		{
			auto d = std::unique_ptr<Deferred>(SessionRequest::createOrUpdate(stateClient, sid, lastIds));

			// safe to not track, since d can't outlive this
			d->finished.connect(boost::bind(&AcceptWorker::sessionCreateOrUpdate_finished, this, d.get(), boost::placeholders::_1));

			deferreds[d.get()] = std::move(d);
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
				// apply ResponseContent filters of all channels
				QStringList allFilters;
				foreach(const Instruct::Channel &c, instruct.channels)
				{
					foreach(const QString &filter, c.filters)
					{
						if((Filter::targets(filter) & Filter::ResponseContent) && !allFilters.contains(filter))
							allFilters += filter;
					}
				}

				Filter::Context fc;
				fc.subscriptionMeta = instruct.meta;

				FilterStack fs(fc, allFilters);
				QByteArray body = fs.process(instruct.response.body);
				if(body.isNull())
				{
					req->respondError("bad-format", QString("filter error: %1").arg(fs.errorMessage()).toUtf8());

					setFinished(true);
					return;
				}

				instruct.response.headers.removeAll("Content-Length");

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
				vresponse["body"] = body;

				result["response"] = vresponse;
			}

			req->respond(result);

			setFinished(true);
			return;
		}

		QByteArray reqFrom = req->from();

		QVariantHash result;
		result["accepted"] = true;
		req->respond(result);

		log_debug("accepting %d requests from %s", requestStates.count(), reqFrom.data());

		if(instruct.holdMode == Instruct::ResponseHold)
		{
			bool conflict = false;
			foreach(const Instruct::Channel &c, instruct.channels)
			{
				if(!c.prevId.isNull())
				{
					QString name = channelPrefix + c.name;

					QString lastId = cs->publishLastIds.value(name);
					if(!lastId.isNull() && lastId != c.prevId)
					{
						log_debug("last ID inconsistency (got=%s, expected=%s), retrying", qPrintable(c.prevId), qPrintable(lastId));
						cs->publishLastIds.remove(name);
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
					QByteArray cid = rs.rid.first + ':' + rs.rid.second;

					needRemoveFromStats.remove(cid);

					int unreportedTime = stats->removeConnection(cid, true, reqFrom);

					RetryRequestPacket::Request rpreq;
					rpreq.rid = rs.rid;
					rpreq.https = rs.isHttps;
					rpreq.peerAddress = rs.peerAddress;
					rpreq.debug = rs.debug;
					rpreq.autoCrossOrigin = rs.autoCrossOrigin;
					rpreq.jsonpCallback = rs.jsonpCallback;
					rpreq.jsonpExtendedResponse = rs.jsonpExtendedResponse;
					if(!stats->connectionSendEnabled())
						rpreq.unreportedTime = unreportedTime;
					rpreq.inSeq = rs.inSeq;
					rpreq.outSeq = rs.outSeq;
					rpreq.outCredits = rs.outCredits;
					rpreq.routerResp = rs.routerResp;
					rpreq.userData = rs.userData;

					rp.requests += rpreq;
				}

				rp.requestData = origRequestData;

				if(haveInspectInfo)
				{
					rp.haveInspectInfo = true;
					rp.inspectInfo.doProxy = inspectInfo.doProxy;
					rp.inspectInfo.sharingKey = inspectInfo.sharingKey;
					rp.inspectInfo.sid = inspectInfo.sid;
					rp.inspectInfo.lastIds = inspectInfo.lastIds;
					rp.inspectInfo.userData = inspectInfo.userData;
				}

				// if prev-id set on channels, set as inspect lastids so the proxy
				//   will pass as Grip-Last in the next request
				foreach(const Instruct::Channel &c, instruct.channels)
				{
					if(!c.prevId.isNull())
					{
						if(!rp.haveInspectInfo)
						{
							rp.haveInspectInfo = true;
							rp.inspectInfo.doProxy = true;
						}

						rp.inspectInfo.lastIds.insert(c.name.toUtf8(), c.prevId.toUtf8());
					}
				}

				rp.route = route.toUtf8();
				rp.retrySeq = stats->lastRetrySeq(reqFrom);

				retryPacketReady(reqFrom, rp);

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
			ss.routerResp = rs.routerResp;
			ss.userData = rs.userData;

			// take over responsibility for request
			ZhttpRequest *httpReq = zhttpIn->createRequestFromState(ss);

			QSet<QString> implicitChannelsSet;
			foreach(const QString &channel, implicitChannels)
				implicitChannelsSet += channel;

			HttpSession::AcceptData adata;
			adata.from = reqFrom;
			adata.requestData = origRequestData;
			adata.logicalPeerAddress = rs.logicalPeerAddress;
			adata.debug = rs.debug;
			adata.isRetry = rs.isRetry;
			adata.autoCrossOrigin = rs.autoCrossOrigin;
			adata.jsonpCallback = rs.jsonpCallback;
			adata.jsonpExtendedResponse = rs.jsonpExtendedResponse;
			adata.unreportedTime = rs.unreportedTime;
			adata.route = route;
			adata.statsRoute = statsRoute;
			adata.channelPrefix = channelPrefix;
			adata.logLevel = logLevel;
			adata.implicitChannels = implicitChannelsSet;
			adata.sid = sid;
			adata.responseSent = responseSent;
			adata.trusted = trusted;
			adata.haveInspectInfo = haveInspectInfo;
			adata.inspectInfo = inspectInfo;

			QByteArray cid = rid.first + ':' + rid.second;
			needRemoveFromStats.remove(cid);

			sessions += std::make_shared<HttpSession>(httpReq, adata, instruct, zhttpOut, stats, updateLimiter, filterLimiter, &cs->publishLastIds, httpSessionUpdateManager, connectionSubscriptionMax);
		}

		// engine should directly connect to this and register the holds
		//   immediately, to avoid a race with the lastId check
		sessionsReady();

		setFinished(true);
	}

	void sessionDetectRulesSet_finished(Deferred *d, const DeferredResult &result)
	{
		deferreds.erase(d);

		if(!result.success)
			log_error("couldn't store detection rules: condition=%d", result.value.toInt());

		afterSetRules();
	}

	void sessionCreateOrUpdate_finished(Deferred *d, const DeferredResult &result)
	{
		deferreds.erase(d);

		if(!result.success)
			log_error("couldn't create/update session: condition=%d", result.value.toInt());

		afterSessionCalls();
	}
};

class Subscription
{
public:
	Subscription(const QString &channel) :
		channel_(channel)
	{
	}

	const QString & channel() const
	{
		return channel_;
	}

	void start()
	{
		timer_ = std::make_unique<Timer>();
		timer_->timeout.connect(boost::bind(&Subscription::timer_timeout, this));
		timer_->setSingleShot(true);
		timer_->start(SUBSCRIBED_DELAY);
	}

	Signal subscribed;

private:
	QString channel_;
	std::unique_ptr<Timer> timer_;

	void timer_timeout()
	{
		subscribed();
	}
};

class HandlerEngine::Private
{
public:
	class PublishAction : public RateLimiter::Action
	{
	public:
		std::weak_ptr<HandlerEngine::Private> ep;
		std::weak_ptr<ClientSession> target;
		PublishItem item;
		QList<QByteArray> exposeHeaders;

		PublishAction(const std::weak_ptr<HandlerEngine::Private> _ep, const std::weak_ptr<ClientSession> _target, const PublishItem &_item, const QList<QByteArray> &_exposeHeaders = QList<QByteArray>()) :
			ep(_ep),
			target(_target),
			item(_item),
			exposeHeaders(_exposeHeaders)
		{
		}

		virtual bool execute()
		{
			auto epl = ep.lock();
			if(!epl)
				return false;

			auto targetl = target.lock();
			if(!targetl)
				return false;

			epl->publishSend(targetl, item, exposeHeaders);
			return true;
		}
	};

	struct WSSessionConnections {
		Connection sendConnection;
		Connection expConnection;
		Connection errorConnection;
	};

	HandlerEngine *q;
	Configuration config;
	std::unique_ptr<ZhttpManager> zhttpIn;
	std::unique_ptr<ZhttpManager> zhttpOut;
	std::unique_ptr<ZrpcManager> inspectServer;
	std::unique_ptr<ZrpcManager> acceptServer;
	std::unique_ptr<ZrpcManager> stateClient;
	std::unique_ptr<ZrpcManager> controlServer;
	std::unique_ptr<ZrpcManager> proxyControlClient;
	std::unique_ptr<QZmq::Socket> inPullSock;
	std::unique_ptr<QZmq::Valve> inPullValve;
	std::unique_ptr<QZmq::Socket> inSubSock;
	std::unique_ptr<QZmq::Valve> inSubValve;
	std::unique_ptr<QZmq::Socket> retrySock;
	std::unique_ptr<QZmq::Socket> wsControlInitSock;
	std::unique_ptr<QZmq::Valve> wsControlInitValve;
	std::unique_ptr<QZmq::Socket> wsControlStreamSock;
	std::unique_ptr<QZmq::Valve> wsControlStreamValve;
	std::unique_ptr<QZmq::Socket> statsSock;
	std::unique_ptr<QZmq::Socket> proxyStatsSock;
	std::unique_ptr<QZmq::Valve> proxyStatsValve;
	std::unique_ptr<SimpleHttpServer> controlHttpServer;
	std::unique_ptr<StatsManager> stats;
	std::unique_ptr<RateLimiter> publishLimiter;
	std::unique_ptr<RateLimiter> updateLimiter;
	std::shared_ptr<RateLimiter> filterLimiter;
	std::shared_ptr<HttpSessionUpdateManager> httpSessionUpdateManager;
	std::unique_ptr<Sequencer> sequencer;
	CommonState cs;
	QSet<InspectWorker*> inspectWorkers;
	QSet<AcceptWorker*> acceptWorkers;
	std::unique_ptr<Deferred> report;
	std::map<Deferred*, std::unique_ptr<Deferred>> deferreds;
	Connection inspectReqReadyConnection;
	Connection acceptReqReadyConnection;
	Connection controlReqReadyConnection;
	Connection controlServerConnection;
	Connection itemReadyConnection;
	map<Subscription*, Connection> subscribedConnection;
	Connection connectionsRefreshedConnection;
	Connection unsubscribedConnection;
	Connection reportedConnection;
	map<WsSession*, WSSessionConnections> wsSessionConnectionMap;
	Connection pullConnection;
	Connection controlInitValveConnection;
	Connection controlStreamValveConnection;
	Connection inSubValveConnection;
	Connection proxyStatConnection;

	Private(HandlerEngine *_q) :
		q(_q)
	{
		qRegisterMetaType<DetectRuleList>();

		publishLimiter = std::make_unique<RateLimiter>();
		updateLimiter = std::make_unique<RateLimiter>();
		filterLimiter = std::make_shared<RateLimiter>();

		httpSessionUpdateManager = std::make_shared<HttpSessionUpdateManager>();

		sequencer = std::make_unique<Sequencer>(&cs.publishLastIds);
		itemReadyConnection = sequencer->itemReady.connect(boost::bind(&Private::sequencer_itemReady, this, boost::placeholders::_1));
	}

	~Private()
	{
		qDeleteAll(inspectWorkers);
		qDeleteAll(acceptWorkers);
		deferreds.clear();
		cs.wsSessions.clear();
		cs.httpSessions.clear();
		qDeleteAll(cs.subs);
	}

	bool start(const Configuration &_config)
	{
		config = _config;

		publishLimiter->setRate(config.messageRate);
		publishLimiter->setHwm(config.messageHwm);

		updateLimiter->setRate(10);
		updateLimiter->setBatchWaitEnabled(true);

		filterLimiter->setRate(100);

		sequencer->setWaitMax(config.messageWait);
		sequencer->setIdCacheTtl(config.idCacheTtl);

		zhttpIn = std::make_unique<ZhttpManager>();
		zhttpIn->setInstanceId(config.instanceId);
		zhttpIn->setServerInStreamSpecs(config.serverInStreamSpecs);
		zhttpIn->setServerOutSpecs(config.serverOutSpecs);

		zhttpOut = std::make_unique<ZhttpManager>();
		zhttpOut->setInstanceId(config.instanceId);
		zhttpOut->setClientOutSpecs(config.clientOutSpecs);
		zhttpOut->setClientOutStreamSpecs(config.clientOutStreamSpecs);
		zhttpOut->setClientInSpecs(config.clientInSpecs);

		log_debug("zhttp in stream: %s", qPrintable(config.serverInStreamSpecs.join(", ")));
		log_debug("zhttp out: %s", qPrintable(config.serverOutSpecs.join(", ")));

		if(!config.inspectSpecs.isEmpty())
		{
			inspectServer = std::make_unique<ZrpcManager>();
			inspectServer->setBind(false);
			inspectServer->setIpcFileMode(config.ipcFileMode);
			inspectReqReadyConnection = inspectServer->requestReady.connect(boost::bind(&Private::inspectServer_requestReady, this));

			if(!inspectServer->setServerSpecs(config.inspectSpecs))
			{
				// zrpcmanager logs error
				return false;
			}

			log_debug("inspect server: %s", qPrintable(config.inspectSpecs.join(", ")));
		}

		if(!config.acceptSpecs.isEmpty())
		{
			acceptServer = std::make_unique<ZrpcManager>();
			acceptServer->setBind(false);
			acceptServer->setIpcFileMode(config.ipcFileMode);
			acceptReqReadyConnection = acceptServer->requestReady.connect(boost::bind(&Private::acceptServer_requestReady, this));

			if(!acceptServer->setServerSpecs(config.acceptSpecs))
			{
				// zrpcmanager logs error
				return false;
			}

			log_debug("accept server: %s", qPrintable(config.acceptSpecs.join(", ")));
		}

		if(!config.stateSpec.isEmpty())
		{
			stateClient = std::make_unique<ZrpcManager>();
			stateClient->setBind(true);
			stateClient->setIpcFileMode(config.ipcFileMode);
			stateClient->setTimeout(STATE_RPC_TIMEOUT);

			if(!stateClient->setClientSpecs(QStringList() << config.stateSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_debug("state client: %s", qPrintable(config.stateSpec));
		}

		if(!config.commandSpec.isEmpty())
		{
			controlServer = std::make_unique<ZrpcManager>();
			controlServer->setBind(true);
			controlServer->setIpcFileMode(config.ipcFileMode);
			controlReqReadyConnection = controlServer->requestReady.connect(boost::bind(&Private::controlServer_requestReady, this));

			if(!controlServer->setServerSpecs(QStringList() << config.commandSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_debug("control server: %s", qPrintable(config.commandSpec));
		}

		if(!config.pushInSpec.isEmpty())
		{
			inPullSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Pull);
			inPullSock->setHwm(DEFAULT_HWM);

			QString errorMessage;
			if(!ZUtil::setupSocket(inPullSock.get(), config.pushInSpec, true, config.ipcFileMode, &errorMessage))
			{
				log_error("%s", qPrintable(errorMessage));
				return false;
			}

			inPullValve = std::make_unique<QZmq::Valve>(inPullSock.get());
			pullConnection = inPullValve->readyRead.connect(boost::bind(&Private::inPull_readyRead, this, boost::placeholders::_1));

			log_debug("in pull: %s", qPrintable(config.pushInSpec));
		}

		if(!config.pushInSubSpecs.isEmpty())
		{
			inSubSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Sub);
			inSubSock->setSendHwm(SUB_SNDHWM);
			inSubSock->setShutdownWaitTime(0);

			QString errorMessage;
			if(!ZUtil::setupSocket(inSubSock.get(), config.pushInSubSpecs, !config.pushInSubConnect, config.ipcFileMode, &errorMessage))
			{
				log_error("%s", qPrintable(errorMessage));
				return false;
			}

			if(config.pushInSubConnect)
			{
				// some sane TCP keep-alive settings
				// idle=30, cnt=6, intvl=5
				inSubSock->setTcpKeepAliveEnabled(true);
				inSubSock->setTcpKeepAliveParameters(30, 6, 5);
			}

			inSubValve = std::make_unique<QZmq::Valve>(inSubSock.get());
			inSubValveConnection = inSubValve->readyRead.connect(boost::bind(&Private::inSub_readyRead, this, boost::placeholders::_1));

			log_debug("in sub: %s", qPrintable(config.pushInSubSpecs.join(", ")));
		}

		if(!config.retryOutSpecs.isEmpty())
		{
			retrySock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);
			retrySock->setImmediateEnabled(true);
			retrySock->setHwm(DEFAULT_HWM);
			retrySock->setShutdownWaitTime(RETRY_WAIT_TIME);
			retrySock->setRouterMandatoryEnabled(true);

			foreach(const QString &spec, config.retryOutSpecs)
			{
				QString errorMessage;
				if(!ZUtil::setupSocket(retrySock.get(), spec, false, config.ipcFileMode, &errorMessage))
				{
					log_error("%s", qPrintable(errorMessage));
					return false;
				}
			}

			log_debug("retry: %s", qPrintable(config.retryOutSpecs.join(", ")));
		}

		if(!config.wsControlInitSpecs.isEmpty() && !config.wsControlStreamSpecs.isEmpty())
		{
			wsControlInitSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Pull);
			wsControlInitSock->setHwm(DEFAULT_HWM);

			foreach(const QString &spec, config.wsControlInitSpecs)
			{
				QString errorMessage;
				if(!ZUtil::setupSocket(wsControlInitSock.get(), spec, false, config.ipcFileMode, &errorMessage))
				{
					log_error("%s", qPrintable(errorMessage));
					return false;
				}
			}

			wsControlInitValve = std::make_unique<QZmq::Valve>(wsControlInitSock.get());
			controlInitValveConnection = wsControlInitValve->readyRead.connect(boost::bind(&Private::wsControlInit_readyRead, this, boost::placeholders::_1));

			log_debug("ws control init: %s", qPrintable(config.wsControlInitSpecs.join(", ")));

			wsControlStreamSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);
			wsControlStreamSock->setIdentity(config.instanceId);
			wsControlStreamSock->setImmediateEnabled(true);
			wsControlStreamSock->setHwm(DEFAULT_HWM);
			wsControlStreamSock->setShutdownWaitTime(WSCONTROL_WAIT_TIME);

			foreach(const QString &spec, config.wsControlStreamSpecs)
			{
				QString errorMessage;
				if(!ZUtil::setupSocket(wsControlStreamSock.get(), spec, false, config.ipcFileMode, &errorMessage))
				{
					log_error("%s", qPrintable(errorMessage));
					return false;
				}
			}

			wsControlStreamValve = std::make_unique<QZmq::Valve>(wsControlStreamSock.get());
			controlStreamValveConnection = wsControlStreamValve->readyRead.connect(boost::bind(&Private::wsControlStream_readyRead, this, boost::placeholders::_1));

			log_debug("ws control stream: %s", qPrintable(config.wsControlStreamSpecs.join(", ")));
		}

		stats = std::make_unique<StatsManager>(config.connectionsMax, config.connectionsMax * config.connectionSubscriptionMax, PROMETHEUS_CONNECTIONS_MAX);
		connectionsRefreshedConnection = stats->connectionsRefreshed.connect(boost::bind(&Private::stats_connectionsRefreshed, this, boost::placeholders::_1));
		unsubscribedConnection = stats->unsubscribed.connect(boost::bind(&Private::stats_unsubscribed, this, boost::placeholders::_1, boost::placeholders::_2));
		reportedConnection = stats->reported.connect(boost::bind(&Private::stats_reported, this, boost::placeholders::_1));

		stats->setConnectionSendEnabled(config.statsConnectionSend);
		stats->setConnectionTtl(config.statsConnectionTtl);
		stats->setSubscriptionTtl(config.statsSubscriptionTtl);
		stats->setSubscriptionLinger(config.subscriptionLinger);
		stats->setReportInterval(config.statsReportInterval);

		if(config.statsFormat == "json")
		{
			stats->setOutputFormat(StatsManager::JsonFormat);
		}
		else
		{
			stats->setOutputFormat(StatsManager::TnetStringFormat);
		}

		if(!config.statsSpec.isEmpty())
		{
			stats->setInstanceId(config.instanceId);
			stats->setIpcFileMode(config.ipcFileMode);

			if(!stats->setSpec(config.statsSpec))
			{
				// statsmanager logs error
				return false;
			}

			log_debug("stats: %s", qPrintable(config.statsSpec));
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

		if(!config.proxyStatsSpecs.isEmpty())
		{
			proxyStatsSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Sub);
			proxyStatsSock->setHwm(DEFAULT_HWM);
			proxyStatsSock->setShutdownWaitTime(0);
			proxyStatsSock->subscribe("");

			foreach(const QString &spec, config.proxyStatsSpecs)
			{
				QString errorMessage;
				if(!ZUtil::setupSocket(proxyStatsSock.get(), spec, false, config.ipcFileMode, &errorMessage))
				{
						log_error("%s", qPrintable(errorMessage));
						return false;
				}
			}

			proxyStatsValve = std::make_unique<QZmq::Valve>(proxyStatsSock.get());
			proxyStatConnection = proxyStatsValve->readyRead.connect(boost::bind(&Private::proxyStats_readyRead, this, boost::placeholders::_1));

			log_debug("proxy stats: %s", qPrintable(config.proxyStatsSpecs.join(", ")));
		}

		if(!config.proxyCommandSpec.isEmpty())
		{
			proxyControlClient = std::make_unique<ZrpcManager>();
			proxyControlClient->setIpcFileMode(config.ipcFileMode);
			proxyControlClient->setTimeout(PROXY_RPC_TIMEOUT);

			if(!proxyControlClient->setClientSpecs(QStringList() << config.proxyCommandSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_debug("proxy control client: %s", qPrintable(config.proxyCommandSpec));
		}

		if(config.pushInHttpPort != -1)
		{
			controlHttpServer = std::make_unique<SimpleHttpServer>(CONTROL_CONNECTIONS_MAX, config.pushInHttpMaxHeadersSize, config.pushInHttpMaxBodySize);
			controlServerConnection = controlHttpServer->requestReady.connect(boost::bind(&Private::controlHttpServer_requestReady, this));
			controlHttpServer->listen(config.pushInHttpAddr, config.pushInHttpPort);

			log_info("http control server: %s:%d", qPrintable(config.pushInHttpAddr.toString()), config.pushInHttpPort);
		}

		if(inPullValve)
			inPullValve->open();
		if(inSubValve)
			inSubValve->open();
		if(wsControlInitValve)
			wsControlInitValve->open();
		if(wsControlStreamValve)
			wsControlStreamValve->open();
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
		// only sequence if someone is listening, because we
		//   clear lastId on unsubscribe and don't want it to
		//   be set again until after a subscription returns

		bool seq = (!item.noSeq && cs.subs.contains(item.channel));

		sequencer->addItem(item, seq);
	}

	void writeRetryPacket(const QByteArray &instanceAddress, const RetryRequestPacket &packet)
	{
		if(!retrySock)
		{
			log_error("retry: can't write, no socket");
			return;
		}

		QVariant vout = packet.toVariant();

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("OUT retry: to=%s %s", instanceAddress.data(), qPrintable(TnetString::variantToString(vout, -1)));

		QList<QByteArray> msg;
		msg += instanceAddress;
		msg += QByteArray();
		msg += TnetString::fromVariant(vout);
		retrySock->write(msg);
	}

	void writeWsControlItems(const QByteArray &instanceAddress, const QList<WsControlPacket::Item> &items)
	{
		if(!wsControlStreamSock)
		{
			log_error("wscontrol: can't write, no socket");
			return;
		}

		WsControlPacket out;
		out.from = config.instanceId;
		out.items = items;

		QVariant vout = out.toVariant();

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("OUT wscontrol: to=%s %s", instanceAddress.data(), qPrintable(TnetString::variantToString(vout, -1)));

		QList<QByteArray> msg;
		msg += instanceAddress;
		msg += QByteArray();
		msg += TnetString::fromVariant(vout);
		wsControlStreamSock->write(msg);
	}

	void addSub(const QString &channel)
	{
		if(!cs.subs.contains(channel))
		{
			Subscription *sub = new Subscription(channel);
			subscribedConnection[sub] = sub->subscribed.connect(boost::bind(&Private::sub_subscribed, this, sub));
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
			subscribedConnection.erase(sub);
			delete sub;

			sequencer->clearPendingForChannel(channel);
			cs.publishLastIds.remove(channel);

			if(inSubSock)
			{
				log_debug("SUB socket unsubscribe: %s", qPrintable(channel));
				inSubSock->unsubscribe(channel.toUtf8());
			}
		}
	}

	void removeWsSession(WsSession *s)
	{
		removeSessionChannels(s);

		log_debug("removed ws session: %s", qPrintable(s->cid));

		wsSessionConnectionMap.erase(s);
		cs.wsSessions.remove(s->cid);
	}

	void httpControlRespond(SimpleHttpRequest *req, int code, const QByteArray &reason, const QString &body, const QByteArray &contentType = QByteArray(), const HttpHeaders &headers = HttpHeaders(), int items = -1)
	{
		HttpHeaders outHeaders = headers;
		if(!contentType.isEmpty())
			outHeaders += HttpHeader("Content-Type", contentType);
		else
			outHeaders += HttpHeader("Content-Type", "text/plain");

		req->respond(code, reason, outHeaders, body.toUtf8());
		req->finished.connect([=] { DeferCall::deleteLater(req); });

		QString msg = QString("control: %1 %2 code=%3 %4").arg(req->requestMethod(), QString::fromUtf8(req->requestUri()), QString::number(code), QString::number(body.size()));
		if(items > -1)
			msg += QString(" items=%1").arg(items);

		log_debug("%s", qPrintable(msg));
	}

	void publishSend(const std::shared_ptr<ClientSession> &target, const PublishItem &item, const QList<QByteArray> &exposeHeaders)
	{
		if(auto hs = std::dynamic_pointer_cast<HttpSession>(target))
			hs->publish(item, exposeHeaders);
		else if(auto s = std::dynamic_pointer_cast<WsSession>(target))
			s->publish(item);
	}

	int blocksForData(int size) const
	{
		if(config.messageBlockSize <= 0)
			return -1;

		return (size + config.messageBlockSize - 1) / config.messageBlockSize;
	}

	void updateSessions(const QString &channel = QString())
	{
		if(!channel.isNull())
		{
			QSet<HttpSession*> sessions = cs.responseSessionsByChannel.value(channel);
			foreach(HttpSession *hs, sessions)
				hs->update();

			sessions = cs.streamSessionsByChannel.value(channel);
			foreach(HttpSession *hs, sessions)
				hs->update();
		}
		else
		{
			foreach(const std::shared_ptr<HttpSession> &hs, cs.httpSessions)
				hs->update();
		}
	}

	void recoverCommand()
	{
		cs.publishLastIds.clear();
		updateSessions();
	}

	void removeSessionChannel(HttpSession *hs, const QString &channel)
	{
		Instruct::HoldMode mode = hs->holdMode();
		assert(mode == Instruct::ResponseHold || mode == Instruct::StreamHold);

		QHash<QString, QSet<HttpSession*> > *sessionsByChannel;
		QString modeStr;

		if(mode == Instruct::ResponseHold)
		{
			sessionsByChannel = &cs.responseSessionsByChannel;
			modeStr = "response";
		}
		else // StreamHold
		{
			sessionsByChannel = &cs.streamSessionsByChannel;
			modeStr = "stream";
		}

		if(!sessionsByChannel->contains(channel))
			return;

		QSet<HttpSession*> &cur = (*sessionsByChannel)[channel];
		if(!cur.contains(hs))
			return;

		cur.remove(hs);

		if(!cur.isEmpty())
		{
			stats->addSubscription(modeStr, channel, cur.count());
		}
		else
		{
			sessionsByChannel->remove(channel);

			// linger the unsub in case client long-polls again
			bool linger = (mode == Instruct::ResponseHold);

			stats->removeSubscription(modeStr, channel, linger);
		}
	}

	void removeSessionChannel(WsSession *s, const QString &channel)
	{
		if(!cs.wsSessionsByChannel.contains(channel))
			return;

		QSet<WsSession*> &cur = cs.wsSessionsByChannel[channel];
		if(!cur.contains(s))
			return;

		cur.remove(s);

		if(!cur.isEmpty())
		{
			stats->addSubscription("ws", channel, cur.count());
		}
		else
		{
			cs.wsSessionsByChannel.remove(channel);

			stats->removeSubscription("ws", channel, false);
		}
	}

	void removeSessionChannels(WsSession *s)
	{
		foreach(const QString &channel, s->channels)
			removeSessionChannel(s, channel);
	}

	static void hs_subscribe_cb(void *data, std::tuple<HttpSession *, const QString &> value)
	{
		Private *self = (Private *)data;

		self->hs_subscribe(std::get<0>(value), std::get<1>(value));
	}

	static void hs_unsubscribe_cb(void *data, std::tuple<HttpSession *, const QString &> value)
	{
		Private *self = (Private *)data;

		self->hs_unsubscribe(std::get<0>(value), std::get<1>(value));
	}

	static void hs_finished_cb(void *data, std::tuple<HttpSession *> value)
	{
		Private *self = (Private *)data;

		self->hs_finished(std::get<0>(value));
	}

	void inspectServer_requestReady()
	{
		if(inspectWorkers.count() >= INSPECT_WORKERS_MAX)
			return;

		ZrpcRequest *req = inspectServer->takeNext();
		if(!req)
			return;

		InspectWorker *w = new InspectWorker(req, stateClient.get(), config.shareAll);

		// safe to not track, since w can't outlive this
		w->finished.connect(boost::bind(&Private::inspectWorker_finished, this, w, boost::placeholders::_1));

		inspectWorkers += w;
	}

	void acceptServer_requestReady()
	{
		if(acceptWorkers.count() >= ACCEPT_WORKERS_MAX)
			return;

		ZrpcRequest *req = acceptServer->takeNext();
		if(!req)
			return;

		if(req->method() == "accept")
		{
			// NOTE: to ensure sequential processing of conn-max packets,
			// we need to process any such packets contained within the
			// accept request immediately before returning to the event loop.
			// the start() call will do this

			AcceptWorker *w = new AcceptWorker(req, stateClient.get(), &cs, zhttpIn.get(), zhttpOut.get(), stats.get(), updateLimiter.get(), filterLimiter, httpSessionUpdateManager, config.connectionSubscriptionMax);

			// safe to not track, since w can't outlive this
			w->finished.connect(boost::bind(&Private::acceptWorker_finished, this, w, boost::placeholders::_1));
			w->sessionsReady.connect(boost::bind(&Private::acceptWorker_sessionsReady, this, w));
			w->retryPacketReady.connect(boost::bind(&Private::acceptWorker_retryPacketReady, this, boost::placeholders::_1, boost::placeholders::_2));

			acceptWorkers += w;

			w->start();
		}
		else if(req->method() == "conn-max")
		{
			QVariantHash args = req->args();

			if(args.contains("conn-max"))
			{
				if(typeId(args["conn-max"]) == QMetaType::QVariantList)
				{
					QVariantList packets = args["conn-max"].toList();

					foreach(const QVariant &data, packets)
					{
						StatsPacket p;
						if(!p.fromVariant("conn-max", data) || p.type != StatsPacket::ConnectionsMax)
							continue;

						stats->processExternalPacket(p, false);
					}
				}
			}

			delete req;
		}
		else
		{
			req->respondError("method-not-found");
			delete req;
		}
	}

	void controlServer_requestReady()
	{
		ZrpcRequest *req = controlServer->takeNext();
		if(!req)
			return;

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("IN command: %s args=%s", qPrintable(req->method()), qPrintable(TnetString::variantToString(req->args(), -1)));

		if(req->method() == "conncheck")
		{
			auto d = std::make_unique<ConnCheckWorker>(req, proxyControlClient.get(), stats.get());

			// safe to not track, since d can't outlive this
			d->finished.connect(boost::bind(&Private::deferred_finished, this, d.get(), boost::placeholders::_1));

			deferreds[d.get()] = std::move(d);
		}
		else if(req->method() == "get-zmq-uris")
		{
			QVariantHash out;
			if(!config.commandSpec.isEmpty())
				out["command"] = config.commandSpec.toUtf8();
			if(!config.pushInSpec.isEmpty())
				out["publish-pull"] = config.pushInSpec.toUtf8();
			if(!config.pushInSubSpecs.isEmpty() && !config.pushInSubConnect)
				out["publish-sub"] = config.pushInSubSpecs[0].toUtf8();
			req->respond(out);
			delete req;
		}
		else if(req->method() == "recover")
		{
			recoverCommand();
			req->respond();
			delete req;
		}
		else if(req->method() == "refresh")
		{
			auto d = std::make_unique<RefreshWorker>(req, proxyControlClient.get(), &cs.wsSessionsByChannel);

			// safe to not track, since d can't outlive this
			d->finished.connect(boost::bind(&Private::deferred_finished, this, d.get(), boost::placeholders::_1));

			deferreds[d.get()] = std::move(d);
		}
		else if(req->method() == "publish")
		{
			QVariantHash args = req->args();

			if(!args.contains("items"))
			{
				req->respondError("bad-request", "Invalid format: object does not contain 'items'");
				delete req;
				return;
			}

			if(typeId(args["items"]) != QMetaType::QVariantList)
			{
				req->respondError("bad-request", "Invalid format: object contains 'items' with wrong type");
				delete req;
				return;
			}

			QVariantList vitems = args["items"].toList();

			bool ok;
			QString errorMessage;
			QList<PublishItem> items = parseItems(vitems, &ok, &errorMessage);
			if(!ok)
			{
				req->respondError("bad-request", QString("Invalid format: %1").arg(errorMessage));
				delete req;
				return;
			}

			req->respond();
			delete req;

			foreach(const PublishItem &item, items)
				handlePublishItem(item);
		}
		else
		{
			req->respondError("method-not-found");
			delete req;
		}
	}

	void sequencer_itemReady(const PublishItem &item)
	{
		QList<HttpSession*> responseSessions;
		QList<HttpSession*> streamSessions;
		QList<WsSession*> wsSessions;
		QSet<QString> sids;

		int largestBlocks = -1;
		if(item.size >= 0)
			largestBlocks = blocksForData(item.size);

		if(item.formats.contains(PublishFormat::HttpResponse))
		{
			if(item.size < 0)
				largestBlocks = qMax(blocksForData(item.formats[PublishFormat::HttpResponse].body.size()), largestBlocks);

			QSet<HttpSession*> sessions = cs.responseSessionsByChannel.value(item.channel);
			foreach(HttpSession *hs, sessions)
			{
				assert(hs->holdMode() == Instruct::ResponseHold);
				assert(hs->channels().contains(item.channel));

				responseSessions += hs;

				if(!hs->sid().isEmpty())
					sids += hs->sid();
			}
		}

		if(item.formats.contains(PublishFormat::HttpStream))
		{
			if(item.size < 0)
				largestBlocks = qMax(blocksForData(item.formats[PublishFormat::HttpStream].body.size()), largestBlocks);

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

				streamSessions += hs;

				if(!hs->sid().isEmpty())
					sids += hs->sid();
			}
		}

		if(item.formats.contains(PublishFormat::WebSocketMessage))
		{
			if(item.size < 0)
				largestBlocks = qMax(blocksForData(item.formats[PublishFormat::WebSocketMessage].body.size()), largestBlocks);

			QSet<WsSession*> wsbc = cs.wsSessionsByChannel.value(item.channel);
			foreach(WsSession *s, wsbc)
			{
				assert(s->channels.contains(item.channel));

				wsSessions += s;

				if(!s->sid.isEmpty())
					sids += s->sid;
			}
		}

		// always add for non-identified route
		stats->addMessageReceived(QByteArray(), largestBlocks);

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

			// FIXME: if bodyPatch is used then body is empty. we should
			//   really be calculating blocks after applying patch

			int blocks;
			if(item.size >= 0)
				blocks = blocksForData(item.size);
			else
				blocks = blocksForData(f.body.size());

			foreach(HttpSession *hsp, responseSessions)
			{
				std::shared_ptr<HttpSession> &hs = cs.httpSessions[hsp->rid()];

				QString statsRoute = hs->statsRoute();

				if(!publishLimiter->addAction(statsRoute, new PublishAction(q->d, hs, i, exposeHeaders), blocks != -1 ? blocks : 1))
				{
					if(!statsRoute.isEmpty())
						log_warning("exceeded publish hwm (%d) for route %s, dropping message", config.messageHwm, qPrintable(statsRoute));
					else
						log_warning("exceeded publish hwm (%d), dropping message", config.messageHwm);
				}

				stats->addMessageSent(statsRoute.toUtf8(), "http-response", blocks);
			}

			stats->addMessage(i.channel, i.id, "http-response", responseSessions.count(), blocks != -1 ? blocks * responseSessions.count() : -1);
		}

		if(!streamSessions.isEmpty())
		{
			PublishItem i = item;
			i.format = item.formats.value(PublishFormat::HttpStream);
			i.formats.clear();

			PublishFormat &f = i.format;

			log_debug("relaying to %d http-stream subscribers", streamSessions.count());

			int blocks;
			if(item.size >= 0)
				blocks = blocksForData(item.size);
			else
				blocks = blocksForData(f.body.size());

			foreach(HttpSession *hsp, streamSessions)
			{
				std::shared_ptr<HttpSession> &hs = cs.httpSessions[hsp->rid()];

				QString statsRoute = hs->statsRoute();

				if(!publishLimiter->addAction(statsRoute, new PublishAction(q->d, hs, i), blocks != -1 ? blocks : 1))
				{
					if(!statsRoute.isEmpty())
						log_warning("exceeded publish hwm (%d) for route %s, dropping message", config.messageHwm, qPrintable(statsRoute));
					else
						log_warning("exceeded publish hwm (%d), dropping message", config.messageHwm);
				}

				stats->addMessageSent(statsRoute.toUtf8(), "http-stream", blocks);
			}

			stats->addMessage(i.channel, i.id, "http-stream", streamSessions.count(), blocks != -1 ? blocks * streamSessions.count() : -1);
		}

		if(!wsSessions.isEmpty())
		{
			PublishItem i = item;
			i.format = item.formats.value(PublishFormat::WebSocketMessage);
			i.formats.clear();

			PublishFormat &f = i.format;

			log_debug("relaying to %d ws-message subscribers", wsSessions.count());

			int blocks;
			if(item.size >= 0)
				blocks = blocksForData(item.size);
			else
				blocks = blocksForData(f.body.size());

			foreach(WsSession *sp, wsSessions)
			{
				std::shared_ptr<WsSession> &s = cs.wsSessions[sp->cid];

				QString statsRoute = s->statsRoute;

				if(!publishLimiter->addAction(statsRoute, new PublishAction(q->d, s, i), blocks != -1 ? blocks : 1))
				{
					if(!statsRoute.isEmpty())
						log_warning("exceeded publish hwm (%d) for route %s, dropping message", config.messageHwm, qPrintable(statsRoute));
					else
						log_warning("exceeded publish hwm (%d), dropping message", config.messageHwm);
				}

				stats->addMessageSent(statsRoute.toUtf8(), "ws-message", blocks);
			}

			stats->addMessage(i.channel, i.id, "ws-message", wsSessions.count(), blocks != -1 ? blocks * wsSessions.count() : -1);
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

			auto d = std::unique_ptr<Deferred>(SessionRequest::updateMany(stateClient.get(), sidLastIds));

			// safe to not track, since d can't outlive this
			d->finished.connect(boost::bind(&Private::sessionUpdateMany_finished, this, d.get(), boost::placeholders::_1));

			deferreds[d.get()] = std::move(d);
		}
	}

private:
	void report_finished(const DeferredResult &result)
	{
		Q_UNUSED(result);

		report.reset();
	}

	void sessionUpdateMany_finished(Deferred *d, const DeferredResult &result)
	{
		deferreds.erase(d);

		if(!result.success)
			log_error("couldn't update session: condition=%d", result.value.toInt());
	}

	void sessionCreateOrUpdate_finished(Deferred *d, const DeferredResult &result)
	{
		deferreds.erase(d);

		if(!result.success)
			log_error("couldn't create/update session: condition=%d", result.value.toInt());
	}

	void inspectWorker_finished(InspectWorker *w, const DeferredResult &result)
	{
		Q_UNUSED(result);

		inspectWorkers.remove(w);
		delete w;

		// try to read again
		inspectServer_requestReady();
	}

	void acceptWorker_finished(AcceptWorker *w, const DeferredResult &result)
	{
		Q_UNUSED(result);

		acceptWorkers.remove(w);
		delete w;

		// try to read again
		acceptServer_requestReady();
	}

	void deferred_finished(Deferred *d, const DeferredResult &result)
	{
		Q_UNUSED(result);

		deferreds.erase(d);
	}
	
	void sub_subscribed(Subscription *sub)
	{
		if(config.updateOnFirstSubscription)
			updateSessions(sub->channel());
	}

	void acceptWorker_sessionsReady(AcceptWorker *w)
	{
		QList<std::shared_ptr<HttpSession>> sessions = w->takeSessions();
		foreach(const std::shared_ptr<HttpSession> &hs, sessions)
		{
			// NOTE: for performance reasons we do not call hs->setParent and
			// instead leave the object unparented

			hs->subscribeCallback().add(Private::hs_subscribe_cb, this);
			hs->unsubscribeCallback().add(Private::hs_unsubscribe_cb, this);
			hs->finishedCallback().add(Private::hs_finished_cb, this);

			cs.httpSessions.insert(hs->rid(), hs);

			hs->start();
		}
	}

	void acceptWorker_retryPacketReady(const QByteArray &instanceAddress, const RetryRequestPacket &packet)
	{
		writeRetryPacket(instanceAddress, packet);
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

				HttpSession *hs = cs.httpSessions.value(rid).get();
				if(hs && !hs->sid().isEmpty())
					sidLastIds[hs->sid()] = LastIds();
			}

			if(!sidLastIds.isEmpty())
			{
				auto d = std::unique_ptr<Deferred>(SessionRequest::updateMany(stateClient.get(), sidLastIds));

				// safe to not track, since d can't outlive this
				d->finished.connect(boost::bind(&Private::sessionUpdateMany_finished, this, d.get(), boost::placeholders::_1));

				deferreds[d.get()] = std::move(d);
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
		all.type = StatsPacket::Report;
		all.connectionsMax = 0;
		all.connectionsMinutes = 0;
		all.messagesReceived = 0;
		all.messagesSent = 0;
		all.httpResponseMessagesSent = 0;
		foreach(const StatsPacket &p, packets)
		{
			all.connectionsMax += qMax(p.connectionsMax, 0);
			all.connectionsMinutes += qMax(p.connectionsMinutes, 0);
			all.messagesReceived += qMax(p.messagesReceived, 0);
			all.messagesSent += qMax(p.messagesSent, 0);
			all.httpResponseMessagesSent += qMax(p.httpResponseMessagesSent, 0);
		}

		report = std::unique_ptr<Deferred>(ControlRequest::report(proxyControlClient.get(), all));

		// safe to not track, since report can't outlive this
		report->finished.connect(boost::bind(&Private::report_finished, this, boost::placeholders::_1));
	}

	QVariant parseJsonOrTnetstring(const QByteArray &message, bool *ok = 0, QString *errorMessage = 0) {
		QVariant data;
		bool ok_;
		if(message.length() > 0 && message[0] == 'J') {
			QJsonParseError e;
			QJsonDocument doc = QJsonDocument::fromJson(message.mid(1), &e);
			if(e.error != QJsonParseError::NoError)
			{
				if(errorMessage)
					*errorMessage = QString("received message with invalid format (json parse failed)");
				if(ok)
					*ok = false;
				return data;
			}

			if(doc.isObject())
			{
				data = doc.object().toVariantMap();
			}
			else
			{
				if(errorMessage)
					*errorMessage = QString("received message with invalid format (not a valid json object)");
				if(ok)
					*ok = false;
				return data;
			}
		}
		else
		{
			int offset = 0;
			if(message.length() > 0 && message[0] == 'T') {
				offset = 1;
			}

			data = TnetString::toVariant(message, offset, &ok_);
			if(!ok_)
			{
				if(errorMessage)
					*errorMessage = QString("received message with invalid format (tnetstring parse failed)");
				if(ok)
					*ok = false;
				return data;
			}
		}
		if(ok)
			*ok = true;
		return data;
	}

	void inPull_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("IN pull: received message with parts != 1, skipping");
			return;
		}

		bool ok;
		QString errorMessage;
		QVariant data = parseJsonOrTnetstring(message[0], &ok, &errorMessage);
		if(!ok)
		{
			log_warning("IN pull: %s, skipping", qPrintable(errorMessage));
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("IN pull: %s", qPrintable(TnetString::variantToString(data, -1)));

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
		QString errorMessage;
		QVariant data = parseJsonOrTnetstring(message[1], &ok, &errorMessage);
		if(!ok) {
			log_warning("IN sub: %s, skipping", qPrintable(errorMessage));
			return;
		}

		QString channel = QString::fromUtf8(message[0]);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("IN sub: channel=%s %s", qPrintable(channel), qPrintable(TnetString::variantToString(data, -1)));

		PublishItem item = PublishItem::fromVariant(data, channel, &ok, &errorMessage);
		if(!ok)
		{
			log_warning("IN sub: received message with invalid format: %s, skipping", qPrintable(errorMessage));
			return;
		}

		handlePublishItem(item);
	}

	void wsControlInit_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("IN wscontrol: received message with parts != 1, skipping");
			return;
		}

		wsControlIn_readyRead(message[0]);
	}

	void wsControlStream_readyRead(const QList<QByteArray> &message)
	{
		QZmq::ReqMessage req(message);

		if(req.content().count() != 1)
		{
			log_warning("IN wscontrol: received message with parts != 1, skipping");
			return;
		}

		wsControlIn_readyRead(req.content()[0]);
	}

	void wsControlIn_readyRead(const QByteArray &message)
	{
		bool ok;
		QVariant data = TnetString::toVariant(message, 0, &ok);
		if(!ok)
		{
			log_warning("IN wscontrol: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("IN wscontrol: %s", qPrintable(TnetString::variantToString(data, -1)));

		WsControlPacket packet;
		if(!packet.fromVariant(data))
		{
			log_warning("IN wscontrol: received message with invalid format, skipping");
			return;
		}

		QStringList createOrUpdateSids;
		QHash<QString, LastIds> updateSids;

		QList<WsControlPacket::Item> outItems;

		foreach(const WsControlPacket::Item &item, packet.items)
		{
			if(item.type != WsControlPacket::Item::Ack && !item.requestId.isEmpty())
			{
				// ack receipt
				WsControlPacket::Item i;
				i.cid = item.cid;
				i.type = WsControlPacket::Item::Ack;
				i.requestId = item.requestId;
				outItems += i;
			}

			if(item.type == WsControlPacket::Item::Here)
			{
				std::shared_ptr<WsSession> s = cs.wsSessions.value(item.cid);
				if(!s)
				{
					s = std::make_shared<WsSession>();
					wsSessionConnectionMap[s.get()] = {
						s->send.connect(boost::bind(&Private::wssession_send, this, boost::placeholders::_1, s.get())),
						s->expired.connect(boost::bind(&Private::wssession_expired, this, s.get())),
						s->error.connect(boost::bind(&Private::wssession_error, this, s.get()))
					};
					s->peer = packet.from;
					s->cid = QString::fromUtf8(item.cid);
					s->ttl = item.ttl;
					s->requestData.uri = item.uri;
					s->zhttpOut = zhttpOut.get();
					s->filterLimiter = filterLimiter;
					s->refreshExpiration();
					cs.wsSessions.insert(s->cid, s);
					log_debug("added ws session: %s", qPrintable(s->cid));
				}

				s->debug = item.debug;
				s->route = item.route;
				s->statsRoute = item.separateStats ? item.route : QString();
				s->targetTrusted = item.trusted;
				s->channelPrefix = QString::fromUtf8(item.channelPrefix);
				if(item.logLevel >= 0)
					s->logLevel = item.logLevel;

				if(!s->sid.isEmpty())
					updateSids[s->sid] = LastIds();

				continue;
			}

			// any other type must be for a known cid
			WsSession *s = cs.wsSessions.value(QString::fromUtf8(item.cid)).get();
			if(!s)
			{
				// send cancel, causing the proxy to close the connection. client
				//   will need to retry to repair
				WsControlPacket::Item i;
				i.cid = item.cid;
				i.type = WsControlPacket::Item::Cancel;
				outItems += i;
				continue;
			}

			if(item.type == WsControlPacket::Item::KeepAlive)
			{
				s->ttl = item.ttl;
				s->refreshExpiration();
			}
			else if(item.type == WsControlPacket::Item::Gone || item.type == WsControlPacket::Item::Cancel)
			{
				removeWsSession(s);
			}
			else if(item.type == WsControlPacket::Item::Grip)
			{
				QJsonParseError e;
				QJsonDocument doc = QJsonDocument::fromJson(item.message, &e);
				if(e.error != QJsonParseError::NoError || (!doc.isObject() && !doc.isArray()))
				{
					log_debug("grip control message is not valid json");
					continue;
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
					continue;
				}

				if(cm.type == WsControlMessage::Subscribe)
				{
					if(s->channels.count() < config.connectionSubscriptionMax)
					{
						if(cm.filters.count() > MESSAGEFILTERSTACK_SIZE_MAX)
						{
							s->sendCloseError(QString("too many filters for channel '%1'").arg(cm.channel));
							continue;
						}

						QString channel = s->channelPrefix + cm.channel;
						s->channels += channel;
						s->channelFilters[channel] = cm.filters;

						if(!cs.wsSessionsByChannel.contains(channel))
							cs.wsSessionsByChannel.insert(channel, QSet<WsSession*>());

						cs.wsSessionsByChannel[channel] += s;

						log_debug("ws session %s subscribed to %s", qPrintable(s->cid), qPrintable(channel));

						stats->addSubscription("ws", channel, cs.wsSessionsByChannel.value(channel).count());
						addSub(channel);

						log_info("subscribe %s channel=%s", qPrintable(s->requestData.uri.toString(QUrl::FullyEncoded)), qPrintable(channel));
					}
					else
					{
						auto routeInfo = LogUtil::RouteInfo(s->route, s->logLevel);
						LogUtil::logForRoute(routeInfo, "wssession: too many subscriptions");
					}
				}
				else if(cm.type == WsControlMessage::Unsubscribe)
				{
					QString channel = s->channelPrefix + cm.channel;

					if(!s->implicitChannels.contains(channel))
					{
						s->channels.remove(channel);
						s->channelFilters.remove(channel);

						removeSessionChannel(s, channel);
					}
				}
				else if(cm.type == WsControlMessage::Detach)
				{
					WsControlPacket::Item i;
					i.cid = item.cid;
					i.type = WsControlPacket::Item::Detach;
					outItems += i;
				}
				else if(cm.type == WsControlMessage::Session)
				{
					if(!cm.sessionId.isEmpty())
					{
						s->sid = cm.sessionId;
						createOrUpdateSids += cm.sessionId;
					}
					else
					{
						s->sid.clear();
					}
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
						QByteArray contentType;
						switch(cm.messageType)
						{
							case WsControlMessage::Text:   contentType = "text"; break;
							case WsControlMessage::Binary: contentType = "binary"; break;
							case WsControlMessage::Ping:   contentType = "ping"; break;
							case WsControlMessage::Pong:   contentType = "pong"; break;
							default: continue; // unrecognized type, ignore
						}

						s->keepAliveType = contentType;
						s->keepAliveMessage = cm.content;

						if(cm.keepAliveMode == "interval")
							i.keepAliveMode = "interval";
						else
							i.keepAliveMode = "idle";

						i.timeout = (cm.timeout > 0 ? cm.timeout : DEFAULT_WS_KEEPALIVE_TIMEOUT);
					}
					else
					{
						s->keepAliveType.clear();
						s->keepAliveMessage.clear();
					}

					outItems += i;
				}
				else if(cm.type == WsControlMessage::SendDelayed)
				{
					QByteArray contentType;
					switch(cm.messageType)
					{
						case WsControlMessage::Text:   contentType = "text"; break;
						case WsControlMessage::Binary: contentType = "binary"; break;
						case WsControlMessage::Ping:   contentType = "ping"; break;
						case WsControlMessage::Pong:   contentType = "pong"; break;
						default: continue; // unrecognized type, ignore
					}

					int timeout = (cm.timeout > 0 ? cm.timeout : DEFAULT_WS_SENDDELAYED_TIMEOUT);

					s->sendDelayed(contentType, cm.content, timeout);
				}
				else if(cm.type == WsControlMessage::FlushDelayed)
				{
					s->flushDelayed();
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

					outItems += i;

					stats->addActivity(s->statsRoute.toUtf8(), 1);
				}
			}
			else if(item.type == WsControlPacket::Item::Subscribe)
			{
				QString channel = QString::fromUtf8(item.channel);
				s->channels += channel;
				s->implicitChannels += channel;

				if(!cs.wsSessionsByChannel.contains(channel))
					cs.wsSessionsByChannel.insert(channel, QSet<WsSession*>());

				cs.wsSessionsByChannel[channel] += s;

				log_debug("ws session %s subscribed to %s", qPrintable(s->cid), qPrintable(channel));

				stats->addSubscription("ws", channel, cs.wsSessionsByChannel.value(channel).count());
				addSub(channel);

				log_info("subscribe %s channel=%s", qPrintable(s->requestData.uri.toString(QUrl::FullyEncoded)), qPrintable(channel));
			}
			else if(item.type == WsControlPacket::Item::Ack)
			{
				int reqId = item.requestId.toInt();
				s->ack(reqId);
			}
		}

		if(!outItems.isEmpty())
			writeWsControlItems(packet.from, outItems);

		if(stateClient)
		{
			foreach(const QString &sid, createOrUpdateSids)
			{
				auto d = std::unique_ptr<Deferred>(SessionRequest::createOrUpdate(stateClient.get(), sid, LastIds()));

				// safe to not track, since d can't outlive this
				d->finished.connect(boost::bind(&Private::sessionCreateOrUpdate_finished, this, d.get(), boost::placeholders::_1));

				deferreds[d.get()] = std::move(d);
			}

			if(!updateSids.isEmpty())
			{
				auto d = std::unique_ptr<Deferred>(SessionRequest::updateMany(stateClient.get(), updateSids));

				// safe to not track, since d can't outlive this
				d->finished.connect(boost::bind(&Private::sessionUpdateMany_finished, this, d.get(), boost::placeholders::_1));

				deferreds[d.get()] = std::move(d);
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

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
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
		else if(p.type == StatsPacket::Counts)
		{
			if(p.requestsReceived > 0)
			{
				// merge with our own stats
				stats->addRequestsReceived(p.requestsReceived);
			}
		}
		else if(p.type == StatsPacket::Connected || p.type == StatsPacket::Disconnected)
		{
			if(stats->connectionSendEnabled())
			{
				// track proxy connections for reporting
				bool localReplaced = stats->processExternalPacket(p, false);

				if(!localReplaced)
				{
					// forward the packet. this will stamp the from field and keep the rest
					stats->sendPacket(p);
				}
			}
		}
		else if(p.type == StatsPacket::Report)
		{
			bool mergeConnectionReport = !stats->connectionSendEnabled();

			// merge into local report and don't forward
			stats->processExternalPacket(p, mergeConnectionReport);
		}
	}

	void controlHttpServer_requestReady()
	{
		SimpleHttpRequest *req = controlHttpServer->takeNext();
		if(!req)
			return;

		QByteArray path = req->requestUri();
		if(path.length() > 1 && path[path.length() - 1] == '/')
			path.truncate(path.length() - 1);

		HttpHeaders headers = req->requestHeaders();

		QByteArray responseContentType;
		if(headers.contains("Accept"))
		{
			foreach(const HttpHeaderParameters &params, headers.getAllAsParameters("Accept"))
			{
				if(params.isEmpty() || params[0].first.isEmpty())
					continue;

				QByteArray type = params[0].first;

				if(type == "text/plain" || type == "text/*" || type == "*/*" || type == "*")
				{
					responseContentType = "text/plain";
				}
				else if(type == "application/json" || type == "application/*")
				{
					responseContentType = "application/json";
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

		if(path == "/")
		{
			httpControlRespond(req, 200, "OK", "Pushpin API\n");
		}
		else if(path == "/publish")
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

				if(typeId(mdata["items"]) != QMetaType::QVariantList)
				{
					httpControlRespond(req, 400, "Bad Request", "Invalid format: object contains 'items' with wrong type\n");
					return;
				}

				vitems = mdata["items"].toList();

				bool ok;
				QString errorMessage;
				QList<PublishItem> items = parseItems(vitems, &ok, &errorMessage);
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
		else if(path == "/recover")
		{
			if(req->requestMethod() == "POST")
			{
				QString message = "Updated";
				if(responseContentType == "application/json")
				{
					QVariantMap obj;
					obj["message"] = message;
					QString body = QJsonDocument(QJsonObject::fromVariantMap(obj)).toJson(QJsonDocument::Compact);
					httpControlRespond(req, 200, "OK", body + "\n", responseContentType, HttpHeaders());
				}
				else // text/plain
				{
					httpControlRespond(req, 200, "OK", message + "\n", responseContentType, HttpHeaders());
				}

				recoverCommand();
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

	void hs_subscribe(HttpSession *hs, const QString &channel)
	{
		Instruct::HoldMode mode = hs->holdMode();
		assert(mode == Instruct::ResponseHold || mode == Instruct::StreamHold);

		QHash<QString, QSet<HttpSession*> > *sessionsByChannel;
		QString modeStr;

		if(mode == Instruct::ResponseHold)
		{
			log_debug("adding response hold on %s", qPrintable(channel));

			sessionsByChannel = &cs.responseSessionsByChannel;
			modeStr = "response";
		}
		else // StreamHold
		{
			log_debug("adding stream hold on %s", qPrintable(channel));

			sessionsByChannel = &cs.streamSessionsByChannel;
			modeStr = "stream";
		}

		if(!sessionsByChannel->contains(channel))
			sessionsByChannel->insert(channel, QSet<HttpSession*>());

		(*sessionsByChannel)[channel] += hs;

		stats->addSubscription(modeStr, channel, sessionsByChannel->value(channel).count());
		addSub(channel);

		QString msg = QString("subscribe %1 channel=%2").arg(hs->requestUri().toString(QUrl::FullyEncoded), channel);
		if(hs->isRetry())
			msg += " retry";

		log_info("%s", qPrintable(msg));
	}

	void hs_unsubscribe(HttpSession *hs, const QString &channel)
	{
		removeSessionChannel(hs, channel);
	}

	void hs_finished(HttpSession *hsp)
	{
		QByteArray addr = hsp->retryToAddress();
		RetryRequestPacket rp = hsp->retryPacket();

		std::shared_ptr<HttpSession> hs = cs.httpSessions.take(hsp->rid());

		hs->subscribeCallback().remove(this);
		hs->unsubscribeCallback().remove(this);
		hs->finishedCallback().remove(this);
		DeferCall::deleteLater(new std::shared_ptr<HttpSession>(hs));

		if(!rp.requests.isEmpty())
			writeRetryPacket(addr, rp);
	}

	void wssession_send(const WsControlPacket::Item &i, WsSession *s)
	{
		writeWsControlItems(s->peer, QList<WsControlPacket::Item>() << i);
	}

	void wssession_expired(WsSession *s)
	{
		removeWsSession(s);
	}

	void wssession_error(WsSession *s)
	{
		log_debug("ws session %s control error", qPrintable(s->cid));

		WsControlPacket::Item i;
		i.cid = s->cid.toUtf8();
		i.type = WsControlPacket::Item::Cancel;

		writeWsControlItems(s->peer, QList<WsControlPacket::Item>() << i);

		removeWsSession(s);
	}
};

HandlerEngine::HandlerEngine()
{
	d = std::make_shared<Private>(this);
}

HandlerEngine::~HandlerEngine() = default;

bool HandlerEngine::start(const Configuration &config)
{
	return d->start(config);
}

void HandlerEngine::reload()
{
	d->reload();
}
