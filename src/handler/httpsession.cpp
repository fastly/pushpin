/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "httpsession.h"

#include <assert.h>
#include <QTimer>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "log.h"
#include "bufferlist.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"
#include "cors.h"
#include "jsonpatch.h"
#include "statsmanager.h"
#include "variantutil.h"
#include "publishitem.h"
#include "publishformat.h"
#include "ratelimiter.h"
#include "publishlastids.h"

#define RETRY_TIMEOUT 1000
#define RETRY_MAX 5
#define RETRY_RAND_MAX 1000
#define UPDATES_PER_ACTION_MAX 100
#define PUBLISH_QUEUE_MAX 100

class HttpSession::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		NotStarted,
		SendingFirstInstructResponse,
		WaitingToUpdate,
		Proxying,
		SendingQueue,
		Holding,
		Closing
	};

	enum Priority
	{
		LowPriority,
		HighPriority
	};

	class UpdateAction : public RateLimiter::Action
	{
	public:
		QSet<HttpSession*> sessions;

		virtual bool execute()
		{
			if(sessions.isEmpty())
				return false;

			foreach(HttpSession *q, sessions)
				q->d->doUpdate();

			return true;
		}
	};

	friend class UpdateAction;

	HttpSession *q;
	State state;
	ZhttpRequest *req;
	AcceptData adata;
	Instruct instruct;
	QHash<QString, Instruct::Channel> channels;
	QTimer *timer;
	QTimer *retryTimer;
	StatsManager *stats;
	ZhttpManager *outZhttp;
	ZhttpRequest *outReq; // for fetching next links
	RateLimiter *updateLimiter;
	PublishLastIds *publishLastIds;
	BufferList firstInstructResponse;
	bool haveOutReqHeaders;
	int sentOutReqData;
	int retries;
	QString errorMessage;
	QUrl currentUri;
	QUrl nextUri;
	bool needUpdate;
	UpdateAction *pendingAction;
	QList<PublishItem> publishQueue;

	Private(HttpSession *_q, ZhttpRequest *_req, const HttpSession::AcceptData &_adata, const Instruct &_instruct, ZhttpManager *_outZhttp, StatsManager *_stats, RateLimiter *_updateLimiter, PublishLastIds *_publishLastIds) :
		QObject(_q),
		q(_q),
		req(_req),
		stats(_stats),
		outZhttp(_outZhttp),
		outReq(0),
		updateLimiter(_updateLimiter),
		publishLastIds(_publishLastIds),
		haveOutReqHeaders(false),
		sentOutReqData(0),
		retries(0),
		needUpdate(false),
		pendingAction(0)
	{
		state = NotStarted;

		req->setParent(this);
		connect(req, &ZhttpRequest::bytesWritten, this, &Private::req_bytesWritten);
		connect(req, &ZhttpRequest::error, this, &Private::req_error);

		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, &Private::timer_timeout);

		retryTimer = new QTimer(this);
		connect(retryTimer, &QTimer::timeout, this, &Private::retryTimer_timeout);
		retryTimer->setSingleShot(true);

		adata = _adata;
		instruct = _instruct;

		currentUri = adata.requestData.uri;

		if(!instruct.nextLink.isEmpty())
			nextUri = currentUri.resolved(instruct.nextLink);
	}

	~Private()
	{
		cleanup();

		timer->disconnect(this);
		timer->setParent(0);
		timer->deleteLater();

		retryTimer->disconnect(this);
		retryTimer->setParent(0);
		retryTimer->deleteLater();
	}

	void start()
	{
		assert(state == NotStarted);

		ZhttpRequest::Rid rid = req->rid();
		stats->addConnection(rid.first + ':' + rid.second, adata.route.toUtf8(), StatsManager::Http, adata.peerAddress, req->requestUri().scheme() == "https", true);

		// need to send initial content?
		if((instruct.holdMode == Instruct::NoHold || instruct.holdMode == Instruct::StreamHold) && !adata.responseSent)
		{
			// send initial response
			HttpHeaders headers = instruct.response.headers;
			headers.removeAll("Content-Length");
			if(adata.autoCrossOrigin)
				Cors::applyCorsHeaders(adata.requestData.headers, &headers);
			req->beginResponse(instruct.response.code, instruct.response.reason, headers);

			if(!instruct.response.body.isEmpty())
			{
				state = SendingFirstInstructResponse;

				firstInstructResponse += instruct.response.body;
				tryWriteFirstInstructResponse();
				return;
			}
		}

		firstInstructResponseDone();
	}

	void update(Priority priority)
	{
		if(state == Proxying || state == SendingQueue)
		{
			// if we are already in the process of updating, flag to update
			//   again after current one finishes
			needUpdate = true;
			return;
		}

		if(state == WaitingToUpdate)
		{
			if(priority == HighPriority)
			{
				// switching to high priority
				cleanupAction();
				state = Holding;
			}
			else
			{
				// already waiting, do nothing
				return;
			}
		}

		if(state != Holding)
			return;

		needUpdate = false;

		if(nextUri.isEmpty())
		{
			// can't update without valid link
			return;
		}

		// turn off timers during update
		timer->stop();
		retryTimer->stop();

		if(priority == HighPriority)
		{
			doUpdate();
		}
		else // LowPriority
		{
			state = WaitingToUpdate;

			QString key = QString::fromUtf8(nextUri.toEncoded());

			UpdateAction *action = static_cast<UpdateAction*>(updateLimiter->lastAction(key));
			if(!action || action->sessions.count() >= UPDATES_PER_ACTION_MAX)
			{
				action = new UpdateAction;
				updateLimiter->addAction(key, action);
			}

			action->sessions += q;
			pendingAction = action;
		}
	}

	void publish(const PublishItem &item, const QList<QByteArray> &exposeHeaders)
	{
		const PublishFormat &f = item.format;

		if(f.type == PublishFormat::HttpResponse)
		{
			if(state != Holding)
				return;

			assert(instruct.holdMode == Instruct::ResponseHold);

			if(f.haveBodyPatch)
				respond(f.code, f.reason, f.headers, f.bodyPatch, exposeHeaders);
			else
				respond(f.code, f.reason, f.headers, f.body, exposeHeaders);
		}
		else if(f.type == PublishFormat::HttpStream)
		{
			if(state == WaitingToUpdate || state == Proxying || state == SendingQueue || state == Holding)
			{
				if(publishQueue.count() < PUBLISH_QUEUE_MAX)
				{
					publishQueue += item;

					if(state == Holding)
						trySendQueue();
				}
				else
				{
					log_debug("httpsession: publish queue at max, dropping");
				}
			}
		}
	}

private:
	void cleanup()
	{
		cleanupAction();

		if(outReq)
		{
			delete outReq;
			outReq = 0;
		}
	}

	void cleanupAction()
	{
		if(pendingAction)
		{
			pendingAction->sessions.remove(q);
			pendingAction = 0;
		}
	}

	void prepareToClose()
	{
		state = Closing;

		publishQueue.clear();
		timer->stop();
		retryTimer->stop();
	}

	void tryWriteFirstInstructResponse()
	{
		int avail = req->writeBytesAvailable();
		if(avail > 0)
		{
			req->writeBody(firstInstructResponse.take(avail));

			if(firstInstructResponse.isEmpty())
				firstInstructResponseDone();
		}
	}

	void firstInstructResponseDone()
	{
		if(instruct.holdMode == Instruct::NoHold)
		{
			// NoHold instruct MUST have had a link to make it this far
			assert(!nextUri.isEmpty());

			doUpdate();
		}
		else // ResponseHold, StreamHold
		{
			prepareToSendQueueOrHold();
		}
	}

	void doUpdate()
	{
		state = Proxying;
		pendingAction = 0;

		requestNextLink();
	}

	void prepareToSendQueueOrHold()
	{
		assert(instruct.holdMode != Instruct::NoHold);

		if(instruct.holdMode == Instruct::StreamHold)
		{
			bool conflict = false;
			foreach(const Instruct::Channel &c, instruct.channels)
			{
				if(!c.prevId.isNull())
				{
					QString name = adata.channelPrefix + c.name;

					QString lastId = publishLastIds->value(name);
					if(!lastId.isNull() && lastId != c.prevId)
					{
						log_debug("lastid inconsistency (got=%s, expected=%s), retrying", qPrintable(c.prevId), qPrintable(lastId));
						publishLastIds->remove(name);
						conflict = true;

						// NOTE: don't exit loop here. we want to clear
						//   the last ids of all conflicting channels
					}
				}
			}

			if(conflict)
			{
				// update expects us to be in Holding state
				state = Holding;

				update(LowPriority);
				return;
			}
		}

		QList<QString> channelsRemoved;
		QHashIterator<QString, Instruct::Channel> it(channels);
		while(it.hasNext())
		{
			it.next();
			const QString &name = it.key();

			bool found = false;
			foreach(const Instruct::Channel &c, instruct.channels)
			{
				if(adata.channelPrefix + c.name == name)
				{
					found = true;
					break;
				}
			}
			if(!found)
				channelsRemoved += name;
		}

		QList<QString> channelsAdded;
		foreach(const Instruct::Channel &c, instruct.channels)
		{
			QString name = adata.channelPrefix + c.name;

			if(!channels.contains(name))
			{
				channelsAdded += name;
				channels.insert(name, c);
			}
			else
			{
				// update prev-id
				channels[name].prevId = c.prevId;
			}
		}

		QPointer<QObject> self = this;

		foreach(const QString &channel, channelsRemoved)
		{
			emit q->unsubscribe(channel);

			assert(self); // deleting here would leak subscriptions/connections
		}

		foreach(const QString &channel, channelsAdded)
		{
			emit q->subscribe(channel);

			assert(self); // deleting here would leak subscriptions/connections
		}

		if(instruct.holdMode == Instruct::ResponseHold)
		{
			state = Holding;

			// set timeout
			if(instruct.timeout >= 0)
			{
				timer->setSingleShot(true);
				timer->start(instruct.timeout * 1000);
			}
		}
		else // StreamHold
		{
			// drop any non-matching queued items
			while(!publishQueue.isEmpty())
			{
				PublishItem &item = publishQueue.first();

				if(!channels.contains(item.channel))
				{
					// we don't care about this channel anymore
					publishQueue.removeFirst();
					continue;
				}

				Instruct::Channel &channel = channels[item.channel];

				if(!channel.prevId.isNull() && channel.prevId != item.prevId)
				{
					// item doesn't follow the hold
					publishQueue.removeFirst();
					continue;
				}

				break;
			}

			if(!publishQueue.isEmpty())
			{
				state = SendingQueue;
				trySendQueue();
			}
			else
			{
				sendQueueDone();
			}
		}
	}

	void trySendQueue()
	{
		assert(instruct.holdMode == Instruct::StreamHold);

		while(!publishQueue.isEmpty() && req->writeBytesAvailable() > 0)
		{
			PublishItem item = publishQueue.takeFirst();

			PublishFormat &f = item.format;

			if(f.close)
			{
				prepareToClose();
				req->endBody();
				break;
			}
			else
			{
				if(!channels.contains(item.channel))
				{
					log_debug("httpsession: received publish for channel with no subscription, dropping");
					continue;
				}

				Instruct::Channel &channel = channels[item.channel];

				if(!channel.prevId.isNull())
				{
					if(channel.prevId != item.prevId)
					{
						publishQueue.clear();

						update(LowPriority);
						break;
					}

					channel.prevId = item.id;
				}

				req->writeBody(f.body);

				// restart keep alive timer
				if(instruct.keepAliveTimeout >= 0)
					timer->start(instruct.keepAliveTimeout * 1000);

				if(!nextUri.isEmpty() && instruct.nextLinkTimeout >= 0)
					retryTimer->start(instruct.nextLinkTimeout * 1000);
			}
		}

		if(state == SendingQueue)
		{
			if(publishQueue.isEmpty())
				sendQueueDone();
		}
		else if(state == Holding)
		{
			if(!publishQueue.isEmpty())
			{
				// if backlogged, turn off timers until we're able to send again
				timer->stop();
				retryTimer->stop();
			}
		}
	}

	void sendQueueDone()
	{
		state = Holding;

		// start keep alive timer
		if(instruct.keepAliveTimeout >= 0)
			timer->start(instruct.keepAliveTimeout * 1000);

		if(!nextUri.isEmpty() && instruct.nextLinkTimeout >= 0)
			retryTimer->start(instruct.nextLinkTimeout * 1000);

		if(needUpdate)
			update(LowPriority);
	}

	void respond(int _code, const QByteArray &_reason, const HttpHeaders &_headers, const QByteArray &_body)
	{
		prepareToClose();

		int code = _code;
		QByteArray reason = _reason;
		HttpHeaders headers = _headers;
		QByteArray body = _body;

		headers.removeAll("Content-Length"); // this will be reset if needed

		if(adata.autoCrossOrigin)
		{
			if(!adata.jsonpCallback.isEmpty())
			{
				if(adata.jsonpExtendedResponse)
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

					body = "/**/" + adata.jsonpCallback + '(' + resultJson + ");\n";
				}
				else
				{
					if(body.endsWith("\r\n"))
						body.truncate(body.size() - 2);
					else if(body.endsWith("\n"))
						body.truncate(body.size() - 1);
					body = "/**/" + adata.jsonpCallback + '(' + body + ");\n";
				}

				headers.removeAll("Content-Type");
				headers += HttpHeader("Content-Type", "application/javascript");
				code = 200;
				reason = "OK";
			}
			else
			{
				Cors::applyCorsHeaders(adata.requestData.headers, &headers);
			}
		}

		req->beginResponse(code, reason, headers);
		req->writeBody(body);
		req->endBody();
	}

	void respond(int code, const QByteArray &reason, const HttpHeaders &_headers, const QByteArray &body, const QList<QByteArray> &exposeHeaders)
	{
		// inherit headers from the timeout response
		HttpHeaders headers = instruct.response.headers;
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
		QByteArray body;

		QJsonParseError e;
		QJsonDocument doc = QJsonDocument::fromJson(instruct.response.body, &e);
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
				vbody = VariantUtil::convertToJsonStyle(vbody);
			if(vbody.isValid() && (vbody.type() == QVariant::Map || vbody.type() == QVariant::List))
			{
				QJsonDocument doc;
				if(vbody.type() == QVariant::Map)
					doc = QJsonDocument(QJsonObject::fromVariantMap(vbody.toMap()));
				else // List
					doc = QJsonDocument(QJsonArray::fromVariantList(vbody.toList()));

				body = doc.toJson(QJsonDocument::Compact);

				if(instruct.response.body.endsWith("\r\n"))
					body += "\r\n";
				else if(instruct.response.body.endsWith("\n"))
					body += '\n';
			}
			else
			{
				log_debug("httpsession: failed to apply JSON patch: %s", qPrintable(errorMessage));
			}
		}
		else
		{
			log_debug("httpsession: failed to parse original response body as JSON");
		}

		respond(code, reason, headers, body, exposeHeaders);
	}

	void doFinish()
	{
		ZhttpRequest::Rid rid = req->rid();

		log_debug("httpsession: cleaning up ('%s', '%s')", rid.first.data(), rid.second.data());

		cleanup();

		QPointer<QObject> self = this;

		QHashIterator<QString, Instruct::Channel> it(channels);
		while(it.hasNext())
		{
			it.next();
			const QString &channel = it.key();

			emit q->unsubscribe(channel);

			assert(self); // deleting here would leak subscriptions/connections
		}

		stats->removeConnection(rid.first + ':' + rid.second, false);

		emit q->finished();
	}

	void requestNextLink()
	{
		log_debug("httpsession: next: %s", qPrintable(instruct.nextLink.toString()));

		if(!outZhttp)
		{
			errorMessage = "Instruct contained link, but handler not configured for outbound requests.";
			QMetaObject::invokeMethod(this, "doError", Qt::QueuedConnection);
			return;
		}

		haveOutReqHeaders = false;
		sentOutReqData = 0;

		outReq = outZhttp->createRequest();
		outReq->setParent(this);
		connect(outReq, &ZhttpRequest::readyRead, this, &Private::outReq_readyRead);
		connect(outReq, &ZhttpRequest::error, this, &Private::outReq_error);

		int currentPort = currentUri.port(currentUri.scheme() == "https" ? 443 : 80);
		int nextPort = nextUri.port(currentUri.scheme() == "https" ? 443 : 80);

		QVariantHash passthroughData;

		// if next link points to the same service as the current request,
		//   then we can assume the network would send the request back to
		//   us, so we can handle it internally. if the link points to a
		//   different service, then we can't make this assumption and need
		//   to make the request over the network. note that such a request
		//   could still end up looping back to us
		if(nextUri.scheme() == currentUri.scheme() && nextUri.host() == currentUri.host() && nextPort == currentPort)
		{
			// use proxy routing
			passthroughData["route"] = true;
		}
		else
		{
			// don't use proxy routing
			passthroughData["route"] = false;
			if(!adata.sigIss.isEmpty())
				passthroughData["sig-iss"] = adata.sigIss;
			if(!adata.sigKey.isEmpty())
				passthroughData["sig-key"] = adata.sigKey;
			if(adata.trusted)
				passthroughData["trusted"] = true;
		}

		// share requests to the same URI
		passthroughData["auto-share"] = true;

		outReq->setPassthroughData(passthroughData);

		HttpHeaders headers;
		foreach(const Instruct::Channel &c, channels.values())
		{
			if(!c.prevId.isNull())
				headers += HttpHeader("Grip-Last", c.name.toUtf8() + "; last-id=" + c.prevId.toUtf8());
		}

		outReq->start("GET", nextUri, headers);
		outReq->endBody();
	}

	void tryProcessOutReq()
	{
		if(outReq)
		{
			if(!haveOutReqHeaders)
				return;

			if(outReq->responseCode() < 200 || outReq->responseCode() >= 300)
			{
				outReq_error();
				return;
			}

			if(outReq->bytesAvailable() > 0)
			{
				int avail = req->writeBytesAvailable();
				if(avail <= 0)
					return;

				QByteArray buf = outReq->readBody(avail);
				req->writeBody(buf);

				sentOutReqData += buf.size();
			}

			if(outReq->bytesAvailable() == 0 && outReq->isFinished())
			{
				HttpResponseData responseData;
				responseData.code = outReq->responseCode();
				responseData.reason = outReq->responseReason();
				responseData.headers = outReq->responseHeaders();

				logRequest(outReq->requestMethod(), outReq->requestUri(), outReq->requestHeaders(), responseData.code, sentOutReqData);

				retries = 0;

				delete outReq;
				outReq = 0;

				bool ok;
				Instruct i = Instruct::fromResponse(responseData, &ok, &errorMessage);
				if(!ok)
				{
					doError();
					return;
				}

				// subsequent response must be non-hold or stream hold
				if(i.holdMode != Instruct::NoHold && i.holdMode != Instruct::StreamHold)
				{
					errorMessage = "Next link returned non-stream hold.";
					doError();
					return;
				}

				instruct = i;

				currentUri = nextUri;

				if(!instruct.nextLink.isEmpty())
					nextUri = currentUri.resolved(instruct.nextLink);
				else
					nextUri.clear();

				if(instruct.holdMode == Instruct::StreamHold)
					prepareToSendQueueOrHold();
			}
		}

		if(state == Proxying && !outReq)
		{
			if(!nextUri.isEmpty())
			{
				if(req->writeBytesAvailable() > 0)
					requestNextLink();
			}
			else
			{
				prepareToClose();

				req->endBody();
			}
		}
	}

	static QString makeLastIdsStr(const HttpHeaders &headers)
	{
		QString out;

		bool first = true;
		foreach(const HttpHeaderParameters &params, headers.getAllAsParameters("Grip-Last"))
		{
			if(!first)
				out += ' ';
			out += QString("#%1=%2").arg(QString::fromUtf8(params[0].first), QString::fromUtf8(params.get("last-id")));
			first = false;
		}

		return out;
	}

	void logRequest(const QString &method, const QUrl &uri, const HttpHeaders &headers, int code, int bodySize)
	{
		QString msg = QString("%1 %2").arg(method, uri.toString(QUrl::FullyEncoded));

		if(!adata.route.isEmpty())
			msg += QString(" route=%1").arg(adata.route);

		msg += QString(" code=%1 %2").arg(QString::number(code), QString::number(bodySize));

		QString lastIdsStr = makeLastIdsStr(headers);
		if(!lastIdsStr.isEmpty())
			msg += ' ' + lastIdsStr;

		log_info("%s", qPrintable(msg));
	}

	void logRequestError(const QString &method, const QUrl &uri, const HttpHeaders &headers)
	{
		QString msg = QString("%1 %2").arg(method, uri.toString(QUrl::FullyEncoded));

		if(!adata.route.isEmpty())
			msg += QString(" route=%1").arg(adata.route);

		QString lastIdsStr = makeLastIdsStr(headers);
		if(!lastIdsStr.isEmpty())
			msg += ' ' + lastIdsStr;

		msg += QString(" error");

		log_info("%s", qPrintable(msg));
	}

private slots:
	void doError()
	{
		prepareToClose();

		if(adata.debug)
			req->writeBody("\n\n" + errorMessage.toUtf8() + '\n');

		req->endBody();
	}

	void req_bytesWritten(int count)
	{
		Q_UNUSED(count);

		if(req->isFinished())
		{
			doFinish();
			return;
		}

		if(state == SendingFirstInstructResponse)
		{
			tryWriteFirstInstructResponse();
		}
		else if(state == Proxying)
		{
			tryProcessOutReq();
		}
		else if(state == SendingQueue || state == Holding)
		{
			trySendQueue();
		}
	}

	void req_error()
	{
		doFinish();
	}

	void outReq_readyRead()
	{
		haveOutReqHeaders = true;

		tryProcessOutReq();
	}

	void outReq_error()
	{
		logRequestError(outReq->requestMethod(), outReq->requestUri(), outReq->requestHeaders());

		delete outReq;
		outReq = 0;

		log_debug("httpsession: failed to retrieve next link");

		// can't retry if we started sending data

		if(sentOutReqData <= 0 && retries < RETRY_MAX)
		{
			int delay = RETRY_TIMEOUT;
			for(int n = 0; n < retries; ++n)
				delay *= 2;
			delay += qrand() % RETRY_RAND_MAX;

			log_debug("httpsession: trying again in %dms", delay);

			++retries;

			retryTimer->start(delay);
			return;
		}
		else
		{
			errorMessage = "Failed to retrieve next link.";
			doError();
		}
	}

	void timer_timeout()
	{
		assert(state == Holding);

		if(instruct.holdMode == Instruct::ResponseHold)
		{
			// send timeout response
			respond(instruct.response.code, instruct.response.reason, instruct.response.headers, instruct.response.body);
		}
		else if(instruct.holdMode == Instruct::StreamHold)
		{
			req->writeBody(instruct.keepAliveData);

			stats->addActivity(adata.route.toUtf8(), 1);
		}
	}

	void retryTimer_timeout()
	{
		if(state == Proxying)
			requestNextLink();
		else if(state == Holding)
			update(LowPriority);
	}
};

HttpSession::HttpSession(ZhttpRequest *req, const HttpSession::AcceptData &adata, const Instruct &instruct, ZhttpManager *zhttpOut, StatsManager *stats, RateLimiter *updateLimiter, PublishLastIds *publishLastIds, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, req, adata, instruct, zhttpOut, stats, updateLimiter, publishLastIds);
}

HttpSession::~HttpSession()
{
	delete d;
}

Instruct::HoldMode HttpSession::holdMode() const
{
	return d->instruct.holdMode;
}

ZhttpRequest::Rid HttpSession::rid() const
{
	return d->req->rid();
}

QUrl HttpSession::requestUri() const
{
	return d->adata.requestData.uri;
}

QString HttpSession::route() const
{
	return d->adata.route;
}

QString HttpSession::sid() const
{
	return d->adata.sid;
}

QHash<QString, Instruct::Channel> HttpSession::channels() const
{
	return d->channels;
}

QHash<QString, QString> HttpSession::meta() const
{
	return d->instruct.meta;
}

void HttpSession::start()
{
	d->start();
}

void HttpSession::update()
{
	d->update(Private::LowPriority);
}

void HttpSession::publish(const PublishItem &item, const QList<QByteArray> &exposeHeaders)
{
	d->publish(item, exposeHeaders);
}

#include "httpsession.moc"
