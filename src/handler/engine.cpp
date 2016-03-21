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
#include "httpserver.h"
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
#include "jsonpatch.h"
#include "cors.h"
#include "responselastids.h"
#include "instruct.h"
#include "conncheckworker.h"

#define DEFAULT_HWM 1000
#define SUB_SNDHWM 0 // infinite
#define RETRY_WAIT_TIME 0
#define WSCONTROL_WAIT_TIME 0
#define STATE_RPC_TIMEOUT 1000

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
	QString sid;
	LastIds lastIds;

	InspectWorker(ZrpcRequest *_req, ZrpcManager *_stateClient, bool _shareAll, QObject *parent = 0) :
		Deferred(parent),
		req(_req),
		stateClient(_stateClient),
		shareAll(_shareAll),
		truncated(false)
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

			if(getSession && stateClient)
			{
				// determine session info
				Deferred *d = SessionRequest::detectRulesGet(stateClient, requestData.uri.host().toUtf8(), requestData.uri.path(QUrl::FullyEncoded).toUtf8(), this);
				connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionDetectRulesGet_finished(const DeferredResult &)));
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

		if(shareAll)
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
				connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionGetLastIds_finished(const DeferredResult &)));
				return;
			}
		}
		else
		{
			// log error but keep going
			log_error("failed to detect session");
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
				log_error("failed to detect session");
			}
		}

		doFinish();
	}
};

class Hold : public QObject
{
	Q_OBJECT

public:
	Instruct::HoldMode mode;
	QHash<QString, Instruct::Channel> channels;
	HttpRequestData requestData;
	HttpResponseData response;
	ZhttpRequest *req;
	QHash<QString, QString> meta;
	bool autoCrossOrigin;
	QByteArray jsonpCallback;
	bool jsonpExtendedResponse;
	QString route;
	QString sid;
	int timeout;
	int keepAliveTimeout;
	QByteArray keepAliveData;
	QTimer *timer;
	StatsManager *stats;

	Hold(ZhttpRequest *_req, StatsManager *_stats, QObject *parent = 0) :
		QObject(parent),
		req(_req),
		autoCrossOrigin(false),
		jsonpExtendedResponse(false),
		timeout(-1),
		keepAliveTimeout(-1),
		stats(_stats)
	{
		req->setParent(this);
		connect(req, SIGNAL(bytesWritten(int)), SLOT(req_bytesWritten(int)));
		connect(req, SIGNAL(error()), SLOT(req_error()));

		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
	}

	~Hold()
	{
		timer->disconnect(this);
		timer->setParent(0);
		timer->deleteLater();
	}

	void start()
	{
		if(mode == Instruct::ResponseHold)
		{
			// set timeout
			if(timeout >= 0)
			{
				timer->setSingleShot(true);
				timer->start(timeout * 1000);
			}
		}
		else // StreamHold
		{
			// send initial response
			response.headers.removeAll("Content-Length");
			if(autoCrossOrigin)
				Cors::applyCorsHeaders(requestData.headers, &response.headers);
			req->beginResponse(response.code, response.reason, response.headers);
			req->writeBody(response.body);

			// start keep alive timer
			if(keepAliveTimeout >= 0)
				timer->start(keepAliveTimeout * 1000);
		}
	}

	void respond(int code, const QByteArray &reason, const HttpHeaders &_headers, const QByteArray &body, const QList<QByteArray> &exposeHeaders)
	{
		assert(mode == Instruct::ResponseHold);

		// inherit headers from the timeout response
		HttpHeaders headers = response.headers;
		foreach(const HttpHeader &h, _headers)
			headers.removeAll(h.first);
		foreach(const HttpHeader &h, _headers)
			headers += h;

		// if Grip-Expose-Headers was provided in the push, apply now
		if(!exposeHeaders.isEmpty())
		{
			for(int n = 0; n < headers.count(); ++n)
			{
				const HttpHeader &h = headers[n];

				bool found = false;
				foreach(const QByteArray &e, exposeHeaders)
				{
					if(qstricmp(h.first.data(), e.data()) == 0)
					{
						found = true;
						break;
					}
				}

				if(found)
				{
					headers.removeAt(n);
					--n; // adjust position
				}
			}
		}

		respond(code, reason, headers, body);
	}

	void respond(int code, const QByteArray &reason, const HttpHeaders &headers, const QVariantList &bodyPatch, const QList<QByteArray> &exposeHeaders)
	{
		assert(mode == Instruct::ResponseHold);

		QByteArray body;

		QJsonParseError e;
		QJsonDocument doc = QJsonDocument::fromJson(response.body, &e);
		if(e.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray()))
		{
			QVariant vbody;
			if(doc.isObject())
				vbody = doc.object().toVariantMap();
			else // isArray
				vbody = doc.array().toVariantList();

			QString errorMessage;
			vbody = JsonPatch::patch(vbody, bodyPatch, &errorMessage);
			if(vbody.isValid())
				vbody = convertToJsonStyle(vbody);
			if(vbody.isValid() && (vbody.type() == QVariant::Map || vbody.type() == QVariant::List))
			{
				QJsonDocument doc;
				if(vbody.type() == QVariant::Map)
					doc = QJsonDocument(QJsonObject::fromVariantMap(vbody.toMap()));
				else // List
					doc = QJsonDocument(QJsonArray::fromVariantList(vbody.toList()));

				body = doc.toJson(QJsonDocument::Compact);

				if(response.body.endsWith("\r\n"))
					body += "\r\n";
				else if(response.body.endsWith("\n"))
					body += '\n';
			}
			else
			{
				log_debug("failed to apply JSON patch: %s", qPrintable(errorMessage));
			}
		}
		else
		{
			log_debug("failed to parse original response body as JSON");
		}

		respond(code, reason, headers, body, exposeHeaders);
	}

	void stream(const QByteArray &content)
	{
		assert(mode == Instruct::StreamHold);

		if(req->writeBytesAvailable() < content.size())
		{
			log_debug("not enough send credits, dropping");
			return;
		}

		req->writeBody(content);

		// restart keep alive timer
		if(keepAliveTimeout >= 0)
			timer->start(keepAliveTimeout * 1000);
	}

	void close()
	{
		assert(mode == Instruct::StreamHold);

		req->endBody();
		timer->stop();
	}

signals:
	void finished();

private:
	void respond(int _code, const QByteArray &_reason, const HttpHeaders &_headers, const QByteArray &_body)
	{
		int code = _code;
		QByteArray reason = _reason;
		HttpHeaders headers = _headers;
		QByteArray body = _body;

		headers.removeAll("Content-Length"); // this will be reset if needed

		if(!jsonpCallback.isEmpty())
		{
			if(jsonpExtendedResponse)
			{
				QVariantMap result;
				result["code"] = code;
				result["reason"] = QString::fromUtf8(reason);

				// need to compact headers into a map
				QVariantMap vheaders;
				foreach(const HttpHeader &h, headers)
				{
					// don't add the same header name twice. we'll collect all values for a single header
					bool found = false;
					QMapIterator<QString, QVariant> it(vheaders);
					while(it.hasNext())
					{
						it.next();
						const QString &name = it.key();

						QByteArray uname = name.toUtf8();
						if(qstricmp(uname.data(), h.first.data()) == 0)
						{
							found = true;
							break;
						}
					}
					if(found)
						continue;

					QList<QByteArray> values = headers.getAll(h.first);
					QString mergedValue;
					for(int n = 0; n < values.count(); ++n)
					{
						mergedValue += QString::fromUtf8(values[n]);
						if(n + 1 < values.count())
							mergedValue += ", ";
					}
					vheaders[h.first] = mergedValue;
				}
				result["headers"] = vheaders;

				result["body"] = QString::fromUtf8(body);

				QByteArray resultJson = QJsonDocument(QJsonObject::fromVariantMap(result)).toJson(QJsonDocument::Compact);

				body = "/**/" + jsonpCallback + '(' + resultJson + ");\n";
			}
			else
			{
				if(body.endsWith("\r\n"))
					body.truncate(body.size() - 2);
				else if(body.endsWith("\n"))
					body.truncate(body.size() - 1);
				body = "/**/" + jsonpCallback + '(' + body + ");\n";
			}

			headers.removeAll("Content-Type");
			headers += HttpHeader("Content-Type", "application/javascript");
			code = 200;
			reason = "OK";
		}
		else if(autoCrossOrigin)
		{
			Cors::applyCorsHeaders(requestData.headers, &headers);
		}

		req->beginResponse(code, reason, headers);
		req->writeBody(body);
		req->endBody();
	}

	void doFinish()
	{
		ZhttpRequest::Rid rid = req->rid();
		stats->removeConnection(rid.first + ':' + rid.second, false);

		emit finished();
	}

private slots:
	void req_bytesWritten(int count)
	{
		Q_UNUSED(count);

		if(!req->isFinished())
			return;

		doFinish();
	}

	void req_error()
	{
		ZhttpRequest::Rid rid = req->rid();
		log_debug("cleaning up subscriber ('%s', '%s')", rid.first.data(), rid.second.data());

		doFinish();
	}

	void timer_timeout()
	{
		if(mode == Instruct::ResponseHold)
		{
			// send timeout response
			respond(response.code, response.reason, response.headers, response.body);
		}
		else // StreamHold
		{
			req->writeBody(keepAliveData);

			stats->addActivity(route.toUtf8(), 1);
		}
	}
};

class WsSession : public QObject
{
	Q_OBJECT

public:
	QString cid;
	QString channelPrefix;
	HttpRequestData requestData;
	QString sid;
	QHash<QString, QString> meta;
	QHash<QString, QStringList> channelFilters; // k=channel, v=list(filters)
	QSet<QString> channels;
	int ttl;
	QTimer *timer;

	WsSession(QObject *parent = 0) :
		QObject(parent)
	{
		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
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

class CommonState
{
public:
	QHash<ZhttpRequest::Rid, Hold*> holds;
	QHash<QString, WsSession*> wsSessions;
	QHash<QString, QSet<Hold*> > responseHoldsByChannel;
	QHash<QString, QSet<Hold*> > streamHoldsByChannel;
	QHash<QString, QSet<WsSession*> > wsSessionsByChannel;
	ResponseLastIds responseLastIds;
	QSet<QString> subs;

	CommonState() :
		responseLastIds(1000000)
	{
	}

	// returns set of channels that should be unsubscribed
	QSet<QString> removeResponseChannels(Hold *hold)
	{
		QSet<QString> out;

		QHashIterator<QString, Instruct::Channel> it(hold->channels);
		while(it.hasNext())
		{
			it.next();
			const QString &channel = it.key();

			if(!responseHoldsByChannel.contains(channel))
				continue;

			QSet<Hold*> &cur = responseHoldsByChannel[channel];
			if(!cur.contains(hold))
				continue;

			cur.remove(hold);

			if(cur.isEmpty())
			{
				responseHoldsByChannel.remove(channel);
				out += channel;
			}
		}

		return out;
	}

	// returns set of channels that should be unsubscribed
	QSet<QString> removeStreamChannels(Hold *hold)
	{
		QSet<QString> out;

		QHashIterator<QString, Instruct::Channel> it(hold->channels);
		while(it.hasNext())
		{
			it.next();
			const QString &channel = it.key();

			if(!streamHoldsByChannel.contains(channel))
				continue;

			QSet<Hold*> &cur = streamHoldsByChannel[channel];
			if(!cur.contains(hold))
				continue;

			cur.remove(hold);

			if(cur.isEmpty())
			{
				streamHoldsByChannel.remove(channel);
				out += channel;
			}
		}

		return out;
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
	StatsManager *stats;
	QString route;
	QString channelPrefix;
	QHash<ZhttpRequest::Rid, RequestState> requestStates;
	HttpRequestData requestData;
	bool haveInspectInfo;
	InspectData inspectInfo;
	HttpResponseData responseData;
	QString sid;
	LastIds lastIds;
	QList<Hold*> holds;

	AcceptWorker(ZrpcRequest *_req, ZrpcManager *_stateClient, CommonState *_cs, ZhttpManager *_zhttpIn, StatsManager *_stats, QObject *parent = 0) :
		Deferred(parent),
		req(_req),
		stateClient(_stateClient),
		cs(_cs),
		zhttpIn(_zhttpIn),
		stats(_stats),
		haveInspectInfo(false)
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
					connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionDetectRulesSet_finished(const DeferredResult &)));
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

	QList<Hold*> takeHolds()
	{
		QList<Hold*> out = holds;
		holds.clear();

		foreach(Hold *hold, out)
			hold->setParent(0);

		return out;
	}

signals:
	void holdsReady();
	void retryPacketReady(const RetryRequestPacket &packet);

private:
	void respondError(const QByteArray &condition)
	{
		req->respondError(condition);
		setFinished(true);
	}

	void afterSetRules()
	{
		if(!sid.isEmpty())
		{
			Deferred *d = SessionRequest::createOrUpdate(stateClient, sid, lastIds, this);
			connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionCreateOrUpdate_finished(const DeferredResult &)));
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
			log_debug("failed to parse accept instructions: %s", qPrintable(errorMessage));

			QVariantHash vresponse;
			vresponse["code"] = 502;
			vresponse["reason"] = QByteArray("Bad Gateway");
			QVariantList vheaders;
			vheaders += QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
			vresponse["headers"] = vheaders;
			vresponse["body"] = QByteArray("Error while proxying to origin.\n");

			QVariantHash result;
			result["response"] = vresponse;
			req->respond(result);

			setFinished(true);
			return;
		}

		if(instruct.holdMode == Instruct::NoHold)
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

			QVariantHash result;
			result["response"] = vresponse;
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
			// check if we need to retry
			bool needRetry = false;
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
						needRetry = true;

						// NOTE: don't exit loop here. we want to clear
						//   the last ids of all conflicting channels
					}
				}
			}

			if(needRetry)
			{
				RetryRequestPacket rp;

				foreach(const RequestState &rs, requestStates)
				{
					RetryRequestPacket::Request rpreq;
					rpreq.rid = rs.rid;
					rpreq.https = rs.isHttps;
					rpreq.peerAddress = rs.peerAddress;
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
			ss.inSeq = rs.inSeq;
			ss.outSeq = rs.outSeq;
			ss.outCredits = rs.outCredits;
			ss.userData = rs.userData;

			// take over responsibility for request
			ZhttpRequest *outReq = zhttpIn->createRequestFromState(ss);

			stats->addConnection(rs.rid.first + ':' + rs.rid.second, route.toUtf8(), StatsManager::Http, rs.peerAddress, rs.isHttps, true);

			Hold *hold = new Hold(outReq, stats, this);
			hold->mode = instruct.holdMode;
			hold->requestData = requestData;
			hold->response = instruct.response;
			hold->autoCrossOrigin = rs.autoCrossOrigin;
			hold->jsonpCallback = rs.jsonpCallback;
			hold->jsonpExtendedResponse = rs.jsonpExtendedResponse;
			hold->timeout = instruct.timeout;
			hold->keepAliveTimeout = instruct.keepAliveTimeout;
			hold->keepAliveData = instruct.keepAliveData;
			hold->sid = sid;
			hold->meta = instruct.meta;

			foreach(const Instruct::Channel &c, instruct.channels)
				hold->channels.insert(channelPrefix + c.name, c);

			holds += hold;
		}

		// engine should directly connect to this and register the holds
		//   immediately, to avoid a race with the lastId check
		emit holdsReady();

		setFinished(true);
	}

private slots:
	void sessionDetectRulesSet_finished(const DeferredResult &result)
	{
		if(!result.success)
			log_debug("couldn't store detection rules");

		afterSetRules();
	}

	void sessionCreateOrUpdate_finished(const DeferredResult &result)
	{
		if(!result.success)
			log_debug("couldn't create/update session");

		afterSessionCalls();
	}
};

class Engine::Private : public QObject
{
	Q_OBJECT

public:
	Engine *q;
	Configuration config;
	ZhttpManager *zhttpIn;
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
	HttpServer *controlHttpServer;
	StatsManager *stats;
	CommonState cs;
	QSet<Deferred*> deferreds;

	Private(Engine *_q) :
		QObject(_q),
		q(_q),
		zhttpIn(0),
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
		stats(0)
	{
		qRegisterMetaType<DetectRuleList>();
	}

	~Private()
	{
		qDeleteAll(deferreds);
		qDeleteAll(cs.wsSessions);
		qDeleteAll(cs.holds);
	}

	bool start(const Configuration &_config)
	{
		config = _config;

		zhttpIn = new ZhttpManager(this);

		zhttpIn->setInstanceId(config.instanceId);
		zhttpIn->setServerInStreamSpecs(config.serverInStreamSpecs);
		zhttpIn->setServerOutSpecs(config.serverOutSpecs);

		log_info("zhttp in stream: %s", qPrintable(config.serverInStreamSpecs.join(", ")));
		log_info("zhttp out: %s", qPrintable(config.serverOutSpecs.join(", ")));

		if(!config.inspectSpec.isEmpty())
		{
			inspectServer = new ZrpcManager(this);
			inspectServer->setBind(false);
			inspectServer->setIpcFileMode(config.ipcFileMode);
			connect(inspectServer, SIGNAL(requestReady()), SLOT(inspectServer_requestReady()));

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
			connect(acceptServer, SIGNAL(requestReady()), SLOT(acceptServer_requestReady()));

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
			connect(controlServer, SIGNAL(requestReady()), SLOT(controlServer_requestReady()));

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
			connect(inPullValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(inPull_readyRead(const QList<QByteArray> &)));

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
			connect(inSubValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(inSub_readyRead(const QList<QByteArray> &)));

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
			connect(wsControlInValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(wsControlIn_readyRead(const QList<QByteArray> &)));

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
		connect(stats, SIGNAL(connectionsRefreshed(const QList<QByteArray> &)), SLOT(stats_connectionsRefreshed(const QList<QByteArray> &)));
		connect(stats, SIGNAL(unsubscribed(const QString &, const QString &)), SLOT(stats_unsubscribed(const QString &, const QString &)));

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
			connect(proxyStatsValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(proxyStats_readyRead(const QList<QByteArray> &)));

			log_info("proxy stats: %s", qPrintable(config.proxyStatsSpec));
		}

		if(!config.proxyCommandSpec.isEmpty())
		{
			proxyControlClient = new ZrpcManager(this);
			proxyControlClient->setIpcFileMode(config.ipcFileMode);

			if(!proxyControlClient->setClientSpecs(QStringList() << config.proxyCommandSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("proxy control client: %s", qPrintable(config.proxyCommandSpec));
		}

		if(config.pushInHttpPort != -1)
		{
			controlHttpServer = new HttpServer(this);
			connect(controlHttpServer, SIGNAL(requestReady()), SLOT(controlHttpServer_requestReady()));
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
		QList<Hold*> responseHolds;
		QList<Hold*> streamHolds;
		QList<WsSession*> wsSessions;
		QSet<QString> sids;
		QSet<QString> responseUnsubs;
		QSet<QString> streamUnsubs;

		if(item.formats.contains(PublishFormat::HttpResponse))
		{
			QSet<Hold*> holds = cs.responseHoldsByChannel.value(item.channel);
			foreach(Hold *hold, holds)
			{
				assert(hold->mode == Instruct::ResponseHold);
				assert(hold->channels.contains(item.channel));

				if(!applyFilters(hold->meta, item.meta, hold->channels[item.channel].filters))
					continue;

				responseUnsubs += cs.removeResponseChannels(hold);
				responseHolds += hold;

				if(!hold->sid.isEmpty())
					sids += hold->sid;
			}

			if(!item.id.isNull())
				cs.responseLastIds.set(item.channel, item.id);
		}

		if(item.formats.contains(PublishFormat::HttpStream))
		{
			QSet<Hold*> holds = cs.streamHoldsByChannel.value(item.channel);
			foreach(Hold *hold, holds)
			{
				assert(hold->mode == Instruct::StreamHold);
				assert(hold->channels.contains(item.channel));

				if(!applyFilters(hold->meta, item.meta, hold->channels[item.channel].filters))
					continue;

				if(item.formats[PublishFormat::HttpStream].close)
					streamUnsubs += cs.removeStreamChannels(hold);

				streamHolds += hold;

				if(!hold->sid.isEmpty())
					sids += hold->sid;
			}
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

		if(!responseHolds.isEmpty())
		{
			PublishFormat f = item.formats.value(PublishFormat::HttpResponse);
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

			log_debug("relaying to %d http-response subscribers", responseHolds.count());

			foreach(Hold *hold, responseHolds)
			{
				if(f.haveBodyPatch)
					hold->respond(f.code, f.reason, f.headers, f.bodyPatch, exposeHeaders);
				else
					hold->respond(f.code, f.reason, f.headers, f.body, exposeHeaders);
			}

			stats->addMessage(item.channel, item.id, "http-response", responseHolds.count());
		}

		if(!streamHolds.isEmpty())
		{
			PublishFormat f = item.formats.value(PublishFormat::HttpStream);

			log_debug("relaying to %d http-stream subscribers", streamHolds.count());

			foreach(Hold *hold, streamHolds)
			{
				if(f.close)
					hold->close();
				else
					hold->stream(f.body);
			}

			stats->addMessage(item.channel, item.id, "http-stream", streamHolds.count());
		}

		if(!wsSessions.isEmpty())
		{
			PublishFormat f = item.formats.value(PublishFormat::WebSocketMessage);

			log_debug("relaying to %d ws-message subscribers", wsSessions.count());

			foreach(WsSession *s, wsSessions)
			{
				WsControlPacket::Item i;
				i.cid = s->cid.toUtf8();
				i.type = WsControlPacket::Item::Send;
				i.contentType = f.binary ? "binary" : "text";
				i.message = f.body;
				writeWsControlItem(i);
			}

			stats->addMessage(item.channel, item.id, "ws-message", wsSessions.count());
		}

		int receivers = responseHolds.count() + streamHolds.count() + wsSessions.count();
		log_info("publish channel=%s receivers=%d", qPrintable(item.channel), receivers);

		foreach(const QString &channel, responseUnsubs)
			stats->removeSubscription("response", channel, true);

		foreach(const QString &channel, streamUnsubs)
			stats->removeSubscription("stream", channel, false);

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
			connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionUpdateMany_finished(const DeferredResult &)));
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
			cs.subs += channel;

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
			cs.subs.remove(channel);

			if(inSubSock)
			{
				log_debug("SUB socket unsubscribe: %s", qPrintable(channel));
				inSubSock->unsubscribe(channel.toUtf8());
			}
		}
	}

	void httpControlRespond(HttpRequest *req, int code, const QByteArray &reason, const QString &body, const QByteArray &contentType = QByteArray(), const HttpHeaders &headers = HttpHeaders(), int items = -1)
	{
		HttpHeaders outHeaders = headers;
		if(!contentType.isEmpty())
			outHeaders += HttpHeader("Content-Type", contentType);
		else
			outHeaders += HttpHeader("Content-Type", "text/plain");

		req->respond(code, reason, outHeaders, body.toUtf8());
		connect(req, SIGNAL(finished()), req, SLOT(deleteLater()));

		QString msg = QString("control: %1 %2 code=%3 %4").arg(req->requestMethod()).arg(QString::fromUtf8(req->requestUri())).arg(code).arg(body.size());
		if(items > -1)
			msg += QString(" items=%1").arg(items);

		log_info("%s", qPrintable(msg));
	}

private slots:
	void inspectServer_requestReady()
	{
		ZrpcRequest *req = inspectServer->takeNext();
		if(!req)
			return;

		InspectWorker *w = new InspectWorker(req, stateClient, config.shareAll, this);
		connect(w, SIGNAL(finished(const DeferredResult &)), SLOT(inspectWorker_finished(const DeferredResult &)));
		deferreds += w;
	}

	void acceptServer_requestReady()
	{
		ZrpcRequest *req = acceptServer->takeNext();
		if(!req)
			return;

		AcceptWorker *w = new AcceptWorker(req, stateClient, &cs, zhttpIn, stats, this);
		connect(w, SIGNAL(finished(const DeferredResult &)), SLOT(acceptWorker_finished(const DeferredResult &)));
		connect(w, SIGNAL(holdsReady()), SLOT(acceptWorker_holdsReady()));
		connect(w, SIGNAL(retryPacketReady(const RetryRequestPacket &)), SLOT(acceptWorker_retryPacketReady(const RetryRequestPacket &)));
		deferreds += w;
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
					connect(s, SIGNAL(expired()), SLOT(wssession_expired()));
					s->cid = QString::fromUtf8(item.cid);
					s->ttl = item.ttl;
					s->requestData.uri = item.uri;
					s->refreshExpiration();
					cs.wsSessions.insert(s->cid, s);
					log_debug("added ws session: %s", qPrintable(s->cid));
				}

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
					continue;
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
			}
		}

		if(stateClient && !updateSids.isEmpty())
		{
			foreach(const QString &sid, updateSids)
			{
				Deferred *d = SessionRequest::createOrUpdate(stateClient, sid, LastIds(), this);
				connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionCreateOrUpdate_finished(const DeferredResult &)));
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

			// just forward the packet. this will stamp the from field and keep the rest
			stats->sendPacket(p);

			// update session
			if(stateClient && !sid.isEmpty() && p.type == StatsPacket::Connected)
			{
				QHash<QString, LastIds> sidLastIds;
				sidLastIds[sid] = LastIds();
				Deferred *d = SessionRequest::updateMany(stateClient, sidLastIds, this);
				connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionUpdateMany_finished(const DeferredResult &)));
				deferreds += d;
				return;
			}
		}
	}

	void controlHttpServer_requestReady()
	{
		HttpRequest *req = controlHttpServer->takeNext();
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
			log_debug("couldn't create/update session");
	}

	void sessionUpdateMany_finished(const DeferredResult &result)
	{
		Deferred *d = (Deferred *)sender();
		deferreds.remove(d);

		if(!result.success)
		{
			log_error("couldn't update session");
		}
	}

	void inspectWorker_finished(const DeferredResult &result)
	{
		Q_UNUSED(result);

		InspectWorker *w = (InspectWorker *)sender();
		deferreds.remove(w);
	}

	void acceptWorker_finished(const DeferredResult &result)
	{
		Q_UNUSED(result);

		AcceptWorker *w = (AcceptWorker *)sender();
		deferreds.remove(w);
	}

	void acceptWorker_holdsReady()
	{
		AcceptWorker *w = (AcceptWorker *)sender();

		QList<Hold*> holds = w->takeHolds();
		foreach(Hold *hold, holds)
		{
			hold->setParent(this);
			connect(hold, SIGNAL(finished()), SLOT(hold_finished()));
			cs.holds.insert(hold->req->rid(), hold);

			QHashIterator<QString, Instruct::Channel> it(hold->channels);
			while(it.hasNext())
			{
				it.next();
				const QString &channel = it.key();

				if(hold->mode == Instruct::ResponseHold)
				{
					log_debug("adding response hold on %s", qPrintable(channel));

					if(!cs.responseHoldsByChannel.contains(channel))
						cs.responseHoldsByChannel.insert(channel, QSet<Hold*>());

					cs.responseHoldsByChannel[channel] += hold;
				}
				else // StreamHold
				{
					log_debug("adding stream hold on %s", qPrintable(channel));

					if(!cs.streamHoldsByChannel.contains(channel))
						cs.streamHoldsByChannel.insert(channel, QSet<Hold*>());

					cs.streamHoldsByChannel[channel] += hold;
				}

				log_info("subscribe %s channel=%s", qPrintable(hold->requestData.uri.toString(QUrl::FullyEncoded)), qPrintable(channel));

				stats->addSubscription(hold->mode == Instruct::ResponseHold ? "response" : "stream", channel);
				addSub(channel);
			}

			hold->start();
		}
	}

	void acceptWorker_retryPacketReady(const RetryRequestPacket &packet)
	{
		writeRetryPacket(packet);
	}

	void hold_finished()
	{
		Hold *hold = (Hold *)sender();

		QSet<QString> responseUnsubs;
		QSet<QString> streamUnsubs;

		if(hold->mode == Instruct::ResponseHold)
			responseUnsubs = cs.removeResponseChannels(hold);
		else if(hold->mode == Instruct::StreamHold)
			streamUnsubs = cs.removeStreamChannels(hold);

		foreach(const QString &channel, responseUnsubs)
			stats->removeSubscription("response", channel, true);

		foreach(const QString &channel, streamUnsubs)
			stats->removeSubscription("stream", channel, false);

		cs.holds.remove(hold->req->rid());
		delete hold;
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

				Hold *hold = cs.holds.value(rid);
				if(hold && !hold->sid.isEmpty())
					sidLastIds[hold->sid] = LastIds();
			}

			if(!sidLastIds.isEmpty())
			{
				Deferred *d = SessionRequest::updateMany(stateClient, sidLastIds, this);
				connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionUpdateMany_finished(const DeferredResult &)));
				deferreds += d;
			}
		}
	}

	void stats_unsubscribed(const QString &mode, const QString &channel)
	{
		// NOTE: this callback may be invoked while looping over certain structures,
		//   so be careful what you touch

		Q_UNUSED(mode);

		if(!cs.responseHoldsByChannel.contains(channel) && !cs.streamHoldsByChannel.contains(channel) && !cs.wsSessionsByChannel.contains(channel))
			removeSub(channel);
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
