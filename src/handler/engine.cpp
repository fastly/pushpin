/*
 * Copyright (C) 2015-2022 Fanout, Inc.
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
#include "rtimer.h"
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

class Subscription;

class CommonState
{
public:
	QHash<ZhttpRequest::Rid, HttpSession*> httpSessions;
	QHash<QString, WsSession*> wsSessions;
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
	Q_OBJECT

public:
	ZrpcRequest *req;
	ZrpcManager *stateClient;
	CommonState *cs;
	ZhttpManager *zhttpIn;
	ZhttpManager *zhttpOut;
	StatsManager *stats;
	RateLimiter *updateLimiter;
	HttpSessionUpdateManager *httpSessionUpdateManager;
	QString route;
	QString statsRoute;
	QString channelPrefix;
	QStringList implicitChannels;
	QByteArray sigIss;
	QByteArray sigKey;
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
	QList<HttpSession*> sessions;
	int connectionSubscriptionMax;

	AcceptWorker(ZrpcRequest *_req, ZrpcManager *_stateClient, CommonState *_cs, ZhttpManager *_zhttpIn, ZhttpManager *_zhttpOut, StatsManager *_stats, RateLimiter *_updateLimiter, HttpSessionUpdateManager *_httpSessionUpdateManager, int _connectionSubscriptionMax, QObject *parent = 0) :
		Deferred(parent),
		req(_req),
		stateClient(_stateClient),
		cs(_cs),
		zhttpIn(_zhttpIn),
		zhttpOut(_zhttpOut),
		stats(_stats),
		updateLimiter(_updateLimiter),
		httpSessionUpdateManager(_httpSessionUpdateManager),
		trusted(false),
		haveInspectInfo(false),
		responseSent(false),
		connectionSubscriptionMax(_connectionSubscriptionMax)
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

			if(args.contains("separate-stats"))
			{
				if(args["separate-stats"].type() != QVariant::Bool)
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
				if(args["channel-prefix"].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				channelPrefix = QString::fromUtf8(args["channel-prefix"].toByteArray());
			}

			if(args.contains("channels"))
			{
				if(args["channels"].type() != QVariant::List)
				{
					respondError("bad-request");
					return;
				}

				QVariantList vchannels = args["channels"].toList();
				foreach(const QVariant &v, vchannels)
				{
					if(v.type() != QVariant::ByteArray)
					{
						respondError("bad-request");
						return;
					}

					implicitChannels += QString::fromUtf8(v.toByteArray());
				}
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

			if(!args.contains("response") || args["response"].type() != QVariant::Hash)
			{
				respondError("bad-request");
				return;
			}

			QVariantHash rd = args["response"].toHash();

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
	static HttpRequestData parseRequestData(const QVariantHash &args, const QString &field)
	{
		if(!args.contains(field) || args[field].type() != QVariant::Hash)
			return HttpRequestData();

		QVariantHash rd = args[field].toHash();

		if(!rd.contains("method") || rd["method"].type() != QVariant::ByteArray)
			return HttpRequestData();

		HttpRequestData out;
		out.method = QString::fromLatin1(rd["method"].toByteArray());

		if(!rd.contains("uri") || rd["uri"].type() != QVariant::ByteArray)
			return HttpRequestData();

		out.uri = QUrl(rd["uri"].toString(), QUrl::StrictMode);
		if(!out.uri.isValid())
			return HttpRequestData();

		if(!rd.contains("headers") || rd["headers"].type() != QVariant::List)
			return HttpRequestData();

		foreach(const QVariant &vheader, rd["headers"].toList())
		{
			if(vheader.type() != QVariant::List)
				return HttpRequestData();

			QVariantList vlist = vheader.toList();
			if(vlist.count() != 2 || vlist[0].type() != QVariant::ByteArray || vlist[1].type() != QVariant::ByteArray)
				return HttpRequestData();

			out.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
		}

		if(!rd.contains("body") || rd["body"].type() != QVariant::ByteArray)
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
				// apply ProxyContent filters of all channels
				QStringList allFilters;
				foreach(const Instruct::Channel &c, instruct.channels)
				{
					foreach(const QString &filter, c.filters)
					{
						if((Filter::targets(filter) & Filter::ProxyContent) && !allFilters.contains(filter))
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
			adata.requestData = origRequestData;
			adata.logicalPeerAddress = rs.logicalPeerAddress;
			adata.debug = rs.debug;
			adata.isRetry = rs.isRetry;
			adata.autoCrossOrigin = rs.autoCrossOrigin;
			adata.jsonpCallback = rs.jsonpCallback;
			adata.jsonpExtendedResponse = rs.jsonpExtendedResponse;
			adata.route = route;
			adata.channelPrefix = channelPrefix;
			adata.implicitChannels = implicitChannels.toSet();
			adata.sid = sid;
			adata.responseSent = responseSent;
			adata.sigIss = sigIss;
			adata.sigKey = sigKey;
			adata.trusted = trusted;
			adata.haveInspectInfo = haveInspectInfo;
			adata.inspectInfo = inspectInfo;

			sessions += new HttpSession(httpReq, adata, instruct, zhttpOut, stats, updateLimiter, &cs->publishLastIds, httpSessionUpdateManager, connectionSubscriptionMax, this);
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
	HttpSessionUpdateManager *httpSessionUpdateManager;
	Sequencer *sequencer;
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

		httpSessionUpdateManager = new HttpSessionUpdateManager(this);

		sequencer = new Sequencer(&cs.publishLastIds, this);
		connect(sequencer, &Sequencer::itemReady, this, &Private::sequencer_itemReady);
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

		// up to 10 timers per connection
		RTimer::init(config.connectionsMax * 10);

		publishLimiter->setRate(config.messageRate);
		publishLimiter->setHwm(config.messageHwm);

		updateLimiter->setRate(10);
		updateLimiter->setBatchWaitEnabled(true);

		sequencer->setWaitMax(config.messageWait);
		sequencer->setIdCacheTtl(config.idCacheTtl);

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

		if(!config.pushInSubSpecs.isEmpty())
		{
			inSubSock = new QZmq::Socket(QZmq::Socket::Sub, this);
			inSubSock->setSendHwm(SUB_SNDHWM);
			inSubSock->setShutdownWaitTime(0);

			QString errorMessage;
			if(!ZUtil::setupSocket(inSubSock, config.pushInSubSpecs, !config.pushInSubConnect, config.ipcFileMode, &errorMessage))
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

			inSubValve = new QZmq::Valve(inSubSock, this);
			connect(inSubValve, &QZmq::Valve::readyRead, this, &Private::inSub_readyRead);

			log_info("in sub: %s", qPrintable(config.pushInSubSpecs.join(", ")));
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

		stats = new StatsManager(config.connectionsMax, config.connectionsMax * config.connectionSubscriptionMax, this);
		connect(stats, &StatsManager::connectionsRefreshed, this, &Private::stats_connectionsRefreshed);
		connect(stats, &StatsManager::unsubscribed, this, &Private::stats_unsubscribed);
		connect(stats, &StatsManager::reported, this, &Private::stats_reported);

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

			log_info("stats: %s", qPrintable(config.statsSpec));
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
			controlHttpServer = new SimpleHttpServer(config.pushInHttpMaxHeadersSize, config.pushInHttpMaxBodySize, this);
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
		// only sequence if someone is listening, because we
		//   clear lastId on unsubscribe and don't want it to
		//   be set again until after a subscription returns

		bool seq = (!item.noSeq && cs.subs.contains(item.channel));

		sequencer->addItem(item, seq);
	}

	void writeRetryPacket(const RetryRequestPacket &packet)
	{
		if(!retrySock)
		{
			log_error("retry: can't write, no socket");
			return;
		}

		QVariant vout = packet.toVariant();

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("OUT retry: %s", qPrintable(TnetString::variantToString(vout, -1)));

		retrySock->write(QList<QByteArray>() << TnetString::fromVariant(vout));
	}

	void writeWsControlItems(const QList<WsControlPacket::Item> &items)
	{
		if(!wsControlOutSock)
		{
			log_error("wscontrol: can't write, no socket");
			return;
		}

		WsControlPacket out;
		out.items = items;

		QVariant vout = out.toVariant();

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
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

		cs.wsSessions.remove(s->cid);
		delete s;
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

			if(f.haveContentFilters)
			{
				// ensure content filters match
				QStringList contentFilters;
				foreach(const QString &f, s->channelFilters[item.channel])
				{
					if(Filter::targets(f) & Filter::MessageContent)
						contentFilters += f;
				}
				if(contentFilters != f.contentFilters)
				{
					QString errorMessage = QString("content filter mismatch: subscription=%1 message=%2").arg(contentFilters.join(","), f.contentFilters.join(","));
					log_debug("%s", qPrintable(errorMessage));
					return;
				}
			}

			Filter::Context fc;
			fc.subscriptionMeta = s->meta;
			fc.publishMeta = item.meta;

			FilterStack filters(fc, s->channelFilters[item.channel]);

			if(filters.sendAction() == Filter::Drop)
				return;

			// TODO: hint support for websockets?
			if(f.action != PublishFormat::Send && f.action != PublishFormat::Close)
				return;

			WsControlPacket::Item i;
			i.cid = s->cid.toUtf8();

			if(f.action == PublishFormat::Send)
			{
				QByteArray body = filters.process(f.body);
				if(body.isNull())
				{
					log_debug("filter error: %s", qPrintable(filters.errorMessage()));
					return;
				}

				i.type = WsControlPacket::Item::Send;

				switch(f.messageType)
				{
					case PublishFormat::Text:   i.contentType = "text"; break;
					case PublishFormat::Binary: i.contentType = "binary"; break;
					case PublishFormat::Ping:   i.contentType = "ping"; break;
					case PublishFormat::Pong:   i.contentType = "pong"; break;
					default: return; // unrecognized type, skip
				}

				i.message = body;
			}
			else if(f.action == PublishFormat::Close)
			{
				i.type = WsControlPacket::Item::Close;
				i.code = f.code;
				i.reason = f.reason;
			}

			writeWsControlItems(QList<WsControlPacket::Item>() << i);
		}
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
			foreach(HttpSession *hs, cs.httpSessions)
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

	static void hs_subscribe_cb(void *data, HttpSession *hs, const QString &channel)
	{
		Private *self = (Private *)data;

		self->hs_subscribe(hs, channel);
	}

	static void hs_unsubscribe_cb(void *data, HttpSession *hs, const QString &channel)
	{
		Private *self = (Private *)data;

		self->hs_unsubscribe(hs, channel);
	}

	static void hs_finished_cb(void *data, HttpSession *hs)
	{
		Private *self = (Private *)data;

		self->hs_finished(hs);
	}

private slots:
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

			foreach(HttpSession *hs, responseSessions)
			{
				QString statsRoute = hs->statsRoute();

				if(!publishLimiter->addAction(statsRoute, new PublishAction(this, hs, i, exposeHeaders), blocks != -1 ? blocks : 1))
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

			foreach(HttpSession *hs, streamSessions)
			{
				QString statsRoute = hs->statsRoute();

				if(!publishLimiter->addAction(statsRoute, new PublishAction(this, hs, i), blocks != -1 ? blocks : 1))
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

			foreach(WsSession *s, wsSessions)
			{
				QString statsRoute = s->statsRoute;

				if(!publishLimiter->addAction(statsRoute, new PublishAction(this, s, i), blocks != -1 ? blocks : 1))
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

			Deferred *d = SessionRequest::updateMany(stateClient, sidLastIds, this);
			connect(d, &Deferred::finished, this, &Private::sessionUpdateMany_finished);
			deferreds += d;
		}
	}

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

		AcceptWorker *w = new AcceptWorker(req, stateClient, &cs, zhttpIn, zhttpOut, stats, updateLimiter, httpSessionUpdateManager, config.connectionSubscriptionMax, this);
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

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("IN command: %s args=%s", qPrintable(req->method()), qPrintable(TnetString::variantToString(req->args(), -1)));

		if(req->method() == "conncheck")
		{
			ConnCheckWorker *w = new ConnCheckWorker(req, proxyControlClient, stats, this);
			connect(w, &ConnCheckWorker::finished, this, &Private::deferred_finished);
			deferreds += w;
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
			RefreshWorker *w = new RefreshWorker(req, proxyControlClient, &cs.wsSessionsByChannel, this);
			connect(w, &RefreshWorker::finished, this, &Private::deferred_finished);
			deferreds += w;
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

			if(args["items"].type() != QVariant::List)
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

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("IN wscontrol: %s", qPrintable(TnetString::variantToString(data, -1)));

		WsControlPacket packet;
		if(!packet.fromVariant(data))
		{
			log_warning("IN wscontrol: received message with invalid format, skipping");
			return;
		}

		QStringList updateSids;

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
				WsSession *s = cs.wsSessions.value(item.cid);
				if(!s)
				{
					s = new WsSession(this);
					connect(s, &WsSession::send, this, &Private::wssession_send);
					connect(s, &WsSession::expired, this, &Private::wssession_expired);
					connect(s, &WsSession::error, this, &Private::wssession_error);
					s->cid = QString::fromUtf8(item.cid);
					s->ttl = item.ttl;
					s->requestData.uri = item.uri;
					s->refreshExpiration();
					cs.wsSessions.insert(s->cid, s);
					log_debug("added ws session: %s", qPrintable(s->cid));
				}

				s->route = item.route;
				s->statsRoute = item.separateStats ? item.route : QString();
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
					if(s->channels.count() < config.connectionSubscriptionMax)
					{
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
						log_warning("ws session %s: too many subscriptions", qPrintable(s->cid));
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
						updateSids += cm.sessionId;
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
			writeWsControlItems(outItems);

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
			QString sid;
			if(p.connectionType == StatsPacket::WebSocket)
			{
				WsSession *s = cs.wsSessions.value(QString::fromUtf8(p.connectionId));
				if(s)
					sid = s->sid;
			}

			// track proxy connections for reporting
			bool localReplaced = stats->processExternalPacket(p);

			if(!localReplaced)
			{
				// forward the packet. this will stamp the from field and keep the rest
				stats->sendPacket(p);
			}

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

				if(mdata["items"].type() != QVariant::List)
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
			// NOTE: for performance reasons we do not call hs->setParent and
			// instead leave the object unparented

			hs->setSubscribeCallback(Private::hs_subscribe_cb, this);
			hs->setUnsubscribeCallback(Private::hs_unsubscribe_cb, this);
			hs->setFinishedCallback(Private::hs_finished_cb, this);

			cs.httpSessions.insert(hs->rid(), hs);

			hs->start();
		}
	}

	void acceptWorker_retryPacketReady(const RetryRequestPacket &packet)
	{
		writeRetryPacket(packet);
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

	void hs_finished(HttpSession *hs)
	{
		RetryRequestPacket rp = hs->retryPacket();

		cs.httpSessions.remove(hs->rid());
		delete hs;

		if(!rp.requests.isEmpty())
			writeRetryPacket(rp);
	}

	void wssession_send(int reqId, const QByteArray &type, const QByteArray &message)
	{
		WsSession *s = (WsSession *)sender();

		WsControlPacket::Item i;
		i.cid = s->cid.toUtf8();
		i.requestId = QByteArray::number(reqId);
		i.type = WsControlPacket::Item::Send;
		i.contentType = type;
		i.message = message;
		i.queue = true;

		writeWsControlItems(QList<WsControlPacket::Item>() << i);
	}

	void wssession_expired()
	{
		WsSession *s = (WsSession *)sender();

		removeWsSession(s);
	}

	void wssession_error()
	{
		WsSession *s = (WsSession *)sender();

		log_debug("ws session %s control error", qPrintable(s->cid));

		WsControlPacket::Item i;
		i.cid = s->cid.toUtf8();
		i.type = WsControlPacket::Item::Cancel;

		writeWsControlItems(QList<WsControlPacket::Item>() << i);

		removeWsSession(s);
	}

	void sub_subscribed()
	{
		Subscription *sub = (Subscription *)sender();

		updateSessions(sub->channel());
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

	void deferred_finished(const DeferredResult &result)
	{
		Q_UNUSED(result);

		Deferred *w = (Deferred *)sender();

		deferreds.remove(w);
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
