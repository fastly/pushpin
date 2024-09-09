/*
 * Copyright (C) 2016-2023 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
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

#include "httpsession.h"

#include <assert.h>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRandomGenerator>
#include "qtcompat.h"
#include "rtimer.h"
#include "log.h"
#include "bufferlist.h"
#include "packet/retryrequestpacket.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"
#include "cors.h"
#include "jsonpatch.h"
#include "statsmanager.h"
#include "logutil.h"
#include "variantutil.h"
#include "publishitem.h"
#include "publishformat.h"
#include "ratelimiter.h"
#include "publishlastids.h"
#include "httpsessionupdatemanager.h"
#include "filterstack.h"

#define RETRY_TIMEOUT 1000
#define RETRY_MAX 5
#define RETRY_RAND_MAX 1000
#define KEEPALIVE_RAND_MAX 1000
#define UPDATES_PER_ACTION_MAX 100
#define PUBLISH_QUEUE_MAX 100

static QByteArray applyBodyPatch(const QByteArray &in, const QVariantList &bodyPatch)
{
	QByteArray body;

	QJsonParseError e;
	QJsonDocument doc = QJsonDocument::fromJson(in, &e);
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
		if(vbody.isValid() && (typeId(vbody) == QMetaType::QVariantMap || typeId(vbody) == QMetaType::QVariantList))
		{
			QJsonDocument doc;
			if(typeId(vbody) == QMetaType::QVariantMap)
				doc = QJsonDocument(QJsonObject::fromVariantMap(vbody.toMap()));
			else // List
				doc = QJsonDocument(QJsonArray::fromVariantList(vbody.toList()));

			body = doc.toJson(QJsonDocument::Compact);

			if(in.endsWith("\r\n"))
				body += "\r\n";
			else if(in.endsWith("\n"))
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

	return body;
}

class HttpSession::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		NotStarted,
		SendingFirstInstructResponse,
		WaitingToUpdate,
		Pausing,
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
	int logLevel;
	QHash<QString, Instruct::Channel> channels;
	RTimer *timer;
	RTimer *retryTimer;
	StatsManager *stats;
	ZhttpManager *outZhttp;
	std::unique_ptr<ZhttpRequest> outReq; // for fetching links
	RateLimiter *updateLimiter;
	PublishLastIds *publishLastIds;
	HttpSessionUpdateManager *updateManager;
	BufferList firstInstructResponse;
	bool haveOutReqHeaders;
	int sentOutReqData;
	int retries;
	QString errorMessage;
	QUrl currentUri;
	QUrl nextUri;
	bool needUpdate;
	Priority needUpdatePriority;
	UpdateAction *pendingAction;
	QList<PublishItem> publishQueue;
	QByteArray retryToAddress;
	RetryRequestPacket retryPacket;
	LogUtil::Config logConfig;
	FilterStack *responseFilters;
	QSet<QString> activeChannels;
	int connectionSubscriptionMax;
	bool connectionStatsActive;
	Callback<std::tuple<HttpSession *, const QString &>> subscribeCallback;
	Callback<std::tuple<HttpSession *, const QString &>> unsubscribeCallback;
	Callback<std::tuple<HttpSession *>> finishedCallback;
	Connection bytesWrittenConnection;
	Connection writeBytesChangedConnection;
	Connection errorConnection;
	Connection pausedConnection;
	Connection readyReadOutConnection;
	Connection errorOutConnection;
	Connection timerConnection;
	Connection retryTimerConnection;

	Private(HttpSession *_q, ZhttpRequest *_req, const HttpSession::AcceptData &_adata, const Instruct &_instruct, ZhttpManager *_outZhttp, StatsManager *_stats, RateLimiter *_updateLimiter, PublishLastIds *_publishLastIds, HttpSessionUpdateManager *_updateManager, int _connectionSubscriptionMax) :
		QObject(_q),
		q(_q),
		req(_req),
		logLevel(LOG_LEVEL_DEBUG),
		stats(_stats),
		outZhttp(_outZhttp),
		updateLimiter(_updateLimiter),
		publishLastIds(_publishLastIds),
		updateManager(_updateManager),
		haveOutReqHeaders(false),
		sentOutReqData(0),
		retries(0),
		needUpdate(false),
		pendingAction(0),
		responseFilters(0),
		connectionSubscriptionMax(_connectionSubscriptionMax),
		connectionStatsActive(true)
	{
		state = NotStarted;

		req->setParent(this);
		bytesWrittenConnection = req->bytesWritten.connect(boost::bind(&Private::req_bytesWritten, this, boost::placeholders::_1));
		writeBytesChangedConnection = req->writeBytesChanged.connect(boost::bind(&Private::req_writeBytesChanged, this));
		errorConnection = req->error.connect(boost::bind(&Private::req_error, this));

		timer = new RTimer;
		timerConnection = timer->timeout.connect(boost::bind(&Private::timer_timeout, this));

		retryTimer = new RTimer;
		retryTimerConnection = retryTimer->timeout.connect(boost::bind(&Private::retryTimer_timeout, this));
		retryTimer->setSingleShot(true);

		adata = _adata;
		instruct = _instruct;

		if(adata.logLevel >= 0)
			logLevel = adata.logLevel;

		currentUri = req->requestUri();

		if(!instruct.nextLink.isEmpty())
			nextUri = currentUri.resolved(instruct.nextLink);
	}

	~Private()
	{
		cleanup();

		timerConnection.disconnect();
		timer->setParent(0);
		timer->deleteLater();

		retryTimerConnection.disconnect();
		retryTimer->setParent(0);
		retryTimer->deleteLater();
	}

	void start()
	{
		assert(state == NotStarted);

		// set up implicit channels
		QPointer<QObject> self = this;
		foreach(const QString &name, adata.implicitChannels)
		{
			if(!channels.contains(name))
			{
				Instruct::Channel c;
				c.name = name;

				channels.insert(name, c);

				subscribeCallback.call({q, name});

				assert(self); // deleting here would leak subscriptions/connections
			}
		}

		if(instruct.channels.count() > connectionSubscriptionMax)
		{
			instruct.channels = instruct.channels.mid(0, connectionSubscriptionMax);

			auto routeInfo = LogUtil::RouteInfo(adata.route, logLevel);
			LogUtil::logForRoute(routeInfo, "httpsession: too many subscriptions");
		}

		// need to send initial content?
		if((instruct.holdMode == Instruct::NoHold || instruct.holdMode == Instruct::StreamHold) && !adata.responseSent)
		{
			// send initial response
			HttpHeaders headers = instruct.response.headers;
			headers.removeAll("Content-Length");
			if(adata.autoCrossOrigin)
				Cors::applyCorsHeaders(req->requestHeaders(), &headers);

			incCounter(Stats::ClientHeaderBytesSent, ZhttpManager::estimateResponseHeaderBytes(instruct.response.code, instruct.response.reason, headers));

			req->beginResponse(instruct.response.code, instruct.response.reason, headers);

			if(!instruct.response.body.isEmpty())
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
				instruct.response.body = fs.process(instruct.response.body);
				if(instruct.response.body.isNull())
				{
					errorMessage = QString("filter error: %1").arg(fs.errorMessage());
					doError();
					return;
				}

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

			if(needUpdate)
			{
				// if needUpdate was already flagged, then raise
				//   priority if needed
				if(priority == HighPriority)
					needUpdatePriority = priority;
			}
			else
			{
				needUpdate = true;
				needUpdatePriority = priority;
			}
			return;
		}

		if(state == WaitingToUpdate)
		{
			if(priority == HighPriority)
			{
				// switching to high priority
				cancelAction();
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

		if(instruct.holdMode != Instruct::ResponseHold && nextUri.isEmpty())
		{
			// can't update without valid link
			return;
		}

		// turn off update timer during update
		updateManager->unregisterSession(q);

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

			if(!channels.contains(item.channel))
			{
				log_debug("httpsession: received publish for channel with no subscription, dropping");
				return;
			}

			Instruct::Channel &channel = channels[item.channel];

			if(!channel.prevId.isNull())
			{
				if(channel.prevId != item.prevId)
				{
					log_debug("last ID inconsistency (got=%s, expected=%s), retrying", qPrintable(item.prevId), qPrintable(channel.prevId));
					publishLastIds->remove(item.channel);

					update(LowPriority);
					return;
				}

				channel.prevId = item.id;
			}

			if(f.haveContentFilters)
			{
				// ensure content filters match
				QStringList contentFilters;
				foreach(const QString &f, channels[item.channel].filters)
				{
					if(Filter::targets(f) & Filter::MessageContent)
						contentFilters += f;
				}
				if(contentFilters != f.contentFilters)
				{
					errorMessage = QString("content filter mismatch: subscription=%1 message=%2").arg(contentFilters.join(","), f.contentFilters.join(","));
					doError();
					return;
				}
			}

			QHash<QString, QString> prevIds;
			QHashIterator<QString, Instruct::Channel> it(channels);
			while(it.hasNext())
			{
				it.next();
				const Instruct::Channel &c = it.value();
				prevIds[c.name] = c.prevId;
			}

			Filter::Context fc;
			fc.prevIds = prevIds;
			fc.subscriptionMeta = instruct.meta;
			fc.publishMeta = item.meta;

			FilterStack fs(fc, channels[item.channel].filters);

			if(fs.sendAction() == Filter::Drop)
				return;

			// NOTE: http-response mode doesn't support a close
			//   action since it's better to send a real response

			if(f.action == PublishFormat::Send)
			{
				QByteArray body;
				if(f.haveBodyPatch)
				{
					body = applyBodyPatch(instruct.response.body, f.bodyPatch);
				}
				else
				{
					body = f.body;
				}

				body = fs.process(body);
				if(body.isNull())
				{
					errorMessage = QString("filter error: %1").arg(fs.errorMessage());
					doError();
					return;
				}

				respond(f.code, f.reason, f.headers, body, exposeHeaders);
			}
			else if(f.action == PublishFormat::Hint)
			{
				update(HighPriority);
			}
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
		cancelActivities();

		if(connectionStatsActive)
		{
			connectionStatsActive = false;

			ZhttpRequest::Rid rid = req->rid();
			QByteArray cid = rid.first + ':' + rid.second;

			stats->removeConnection(cid, false);
		}
	}

	void cleanupOutReq()
	{
		readyReadOutConnection.disconnect();
		errorOutConnection.disconnect();
		outReq.reset();

		delete responseFilters;
		responseFilters = 0;
	}

	void cancelAction()
	{
		if(pendingAction)
		{
			pendingAction->sessions.remove(q);
			pendingAction = 0;
		}
	}

	void cancelActivities()
	{
		cleanupOutReq();
		cancelAction();

		publishQueue.clear();

		timer->stop();
		retryTimer->stop();

		updateManager->unregisterSession(q);
	}

	void setupKeepAlive()
	{
		if(instruct.keepAliveTimeout >= 0)
		{
			int timeout = instruct.keepAliveTimeout * 1000;
			timeout = qMax(timeout - (int)(QRandomGenerator::global()->generate() % KEEPALIVE_RAND_MAX), 0);
			timer->setSingleShot(true);
			timer->start(timeout);
		}
	}

	void adjustKeepAlive()
	{
		// if idle mode, restart the timer. else leave alone
		if(timer && instruct.keepAliveMode == Instruct::Idle)
			setupKeepAlive();
	}

	void prepareToClose()
	{
		state = Closing;

		cancelActivities();
	}

	void tryWriteFirstInstructResponse()
	{
		int avail = req->writeBytesAvailable();
		if(avail > 0)
		{
			writeBody(firstInstructResponse.take(avail));

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
			prepareToSendQueueOrHold(true);
		}
	}

	void doUpdate()
	{
		pendingAction = 0;

		if(instruct.holdMode == Instruct::ResponseHold)
		{
			state = Pausing;

			// stop activity while pausing
			timer->stop();

			pausedConnection = req->paused.connect(boost::bind(&Private::req_paused, this));
			req->pause();
		}
		else
		{
			state = Proxying;

			requestNextLink();
		}
	}

	void prepareToSendQueueOrHold(bool first = false)
	{
		assert(instruct.holdMode != Instruct::NoHold);

		if(instruct.holdMode == Instruct::StreamHold)
		{
			// if prev ids used but not next link, error out
			if(nextUri.isEmpty())
			{
				foreach(const Instruct::Channel &c, instruct.channels)
				{
					if(!c.prevId.isNull())
					{
						errorMessage = QString("channel '%1' specifies prev-id, but no next link found").arg(c.name);
						doError();
						return;
					}
				}
			}
		}

		QList<QString> channelsRemoved;
		QHashIterator<QString, Instruct::Channel> it(channels);
		while(it.hasNext())
		{
			it.next();
			const QString &name = it.key();

			if(adata.implicitChannels.contains(name))
				continue;

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
			{
				channelsRemoved += name;
				channels.remove(name);
			}
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
				// update channel properties
				channels[name].prevId = c.prevId;
				channels[name].filters = c.filters;
			}
		}

		QPointer<QObject> self = this;

		foreach(const QString &channel, channelsRemoved)
		{
			unsubscribeCallback.call({q, channel});

			assert(self); // deleting here would leak subscriptions/connections
		}

		foreach(const QString &channel, channelsAdded)
		{
			subscribeCallback.call({q, channel});

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
			// if conflict on first hold, immediately recover. we don't
			//   do this on subsequent holds because there may be queued
			//   messages available to resolve the conflict
			if(first)
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
							log_debug("last ID inconsistency (got=%s, expected=%s), retrying", qPrintable(c.prevId), qPrintable(lastId));
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
					log_debug("last ID inconsistency (got=%s, expected=%s), retrying", qPrintable(item.prevId), qPrintable(channel.prevId));
					publishLastIds->remove(item.channel);

					publishQueue.clear();

					update(LowPriority);
					break;
				}

				channel.prevId = item.id;
			}

			PublishFormat &f = item.format;

			if(f.haveContentFilters)
			{
				// ensure content filters match
				QStringList contentFilters;
				foreach(const QString &f, channels[item.channel].filters)
				{
					if(Filter::targets(f) & Filter::MessageContent)
						contentFilters += f;
				}
				if(contentFilters != f.contentFilters)
				{
					errorMessage = QString("content filter mismatch: subscription=%1 message=%2").arg(contentFilters.join(","), f.contentFilters.join(","));
					doError();
					break;
				}
			}

			QHash<QString, QString> prevIds;
			QHashIterator<QString, Instruct::Channel> it(channels);
			while(it.hasNext())
			{
				it.next();
				const Instruct::Channel &c = it.value();
				prevIds[c.name] = c.prevId;
			}

			Filter::Context fc;
			fc.prevIds = prevIds;
			fc.subscriptionMeta = instruct.meta;
			fc.publishMeta = item.meta;

			FilterStack fs(fc, channels[item.channel].filters);

			if(fs.sendAction() == Filter::Drop)
				continue;

			if(f.action == PublishFormat::Send)
			{
				QByteArray body = fs.process(f.body);
				if(body.isNull())
				{
					errorMessage = QString("filter error: %1").arg(fs.errorMessage());
					doError();
					break;
				}

				writeBody(body);

				// restart keep alive timer
				adjustKeepAlive();

				if(!nextUri.isEmpty() && instruct.nextLinkTimeout >= 0)
				{
					activeChannels += item.channel;
					if(activeChannels.count() == channels.count())
					{
						activeChannels.clear();

						updateManager->registerSession(q, instruct.nextLinkTimeout, nextUri);
					}
				}
			}
			else if(f.action == PublishFormat::Hint)
			{
				// clear queue since any items will be redundant
				publishQueue.clear();

				update(HighPriority);
				break;
			}
			else if(f.action == PublishFormat::Close)
			{
				prepareToClose();
				req->endBody();
				break;
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
				updateManager->unregisterSession(q);
			}
		}
	}

	void sendQueueDone()
	{
		state = Holding;

		activeChannels.clear();

		// start keep alive timer, if it wasn't started already
		if(!timer->isActive())
			setupKeepAlive();

		if(!nextUri.isEmpty() && instruct.nextLinkTimeout >= 0)
			updateManager->registerSession(q, instruct.nextLinkTimeout, nextUri);

		if(needUpdate)
			update(needUpdatePriority);
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
				Cors::applyCorsHeaders(req->requestHeaders(), &headers);
			}
		}

		incCounter(Stats::ClientHeaderBytesSent, ZhttpManager::estimateResponseHeaderBytes(code, reason, headers));

		req->beginResponse(code, reason, headers);
		writeBody(body);
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

	void doFinish(bool retry = false)
	{
		cancelActivities();

		ZhttpRequest::Rid rid = req->rid();
		QByteArray cid = rid.first + ':' + rid.second;

		QPointer<QObject> self = this;

		QHashIterator<QString, Instruct::Channel> it(channels);
		while(it.hasNext())
		{
			it.next();
			const QString &channel = it.key();

			unsubscribeCallback.call({q, channel});

			assert(self); // deleting here would leak subscriptions/connections
		}

		if(retry)
		{
			// refresh before remove, to ensure transition
			stats->refreshConnection(cid);

			connectionStatsActive = false;

			int unreportedTime = stats->removeConnection(cid, true, adata.from);

			ZhttpRequest::ServerState ss = req->serverState();

			RetryRequestPacket rp;

			RetryRequestPacket::Request rpreq;
			rpreq.rid = rid;
			rpreq.https = (req->requestUri().scheme() == "https");
			rpreq.peerAddress = req->peerAddress();
			rpreq.debug = adata.debug;
			rpreq.autoCrossOrigin = adata.autoCrossOrigin;
			rpreq.jsonpCallback = adata.jsonpCallback;
			rpreq.jsonpExtendedResponse = adata.jsonpExtendedResponse;
			if(!stats->connectionSendEnabled())
				rpreq.unreportedTime = unreportedTime;
			rpreq.inSeq = ss.inSeq;
			rpreq.outSeq = ss.outSeq;
			rpreq.outCredits = ss.outCredits;
			rpreq.userData = ss.userData;

			rp.requests += rpreq;

			rp.requestData = adata.requestData;

			if(adata.haveInspectInfo)
			{
				rp.haveInspectInfo = true;
				rp.inspectInfo.doProxy = adata.inspectInfo.doProxy;
				rp.inspectInfo.sharingKey = adata.inspectInfo.sharingKey;
				rp.inspectInfo.sid = adata.inspectInfo.sid;
				rp.inspectInfo.lastIds = adata.inspectInfo.lastIds;
				rp.inspectInfo.userData = adata.inspectInfo.userData;
			}

			// if prev-id set on channels, set as inspect lastids so the proxy
			//   will pass as Grip-Last in the next request
			QHashIterator<QString, Instruct::Channel> it(channels);
			while(it.hasNext())
			{
				it.next();
				const Instruct::Channel &c = it.value();

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

			rp.route = adata.route.toUtf8();
			rp.retrySeq = stats->lastRetrySeq(adata.from);

			retryToAddress = adata.from;
			retryPacket = rp;
		}
		else
		{
			connectionStatsActive = false;

			stats->removeConnection(cid, false);
		}

		log_debug("httpsession: cleaning up %s", cid.data());
		cleanup();

		finishedCallback.call({q});
	}

	void prepareOutReq(const QUrl &destUri, bool autoShare = false)
	{
		haveOutReqHeaders = false;
		sentOutReqData = 0;

		outReq.reset(outZhttp->createRequest());
		outReq->setParent(this);
		readyReadOutConnection = outReq->readyRead.connect(boost::bind(&Private::outReq_readyRead, this));
		errorOutConnection = outReq->error.connect(boost::bind(&Private::outReq_error, this));

		int currentPort = currentUri.port(currentUri.scheme() == "https" ? 443 : 80);
		int destPort = destUri.port(destUri.scheme() == "https" ? 443 : 80);

		QVariantHash passthroughData;

		passthroughData["route"] = adata.route.toUtf8();

		// if next link points to the same service as the current request,
		//   then we can assume the network would send the request back to
		//   us, so we can handle it internally. if the link points to a
		//   different service, then we can't make this assumption and need
		//   to make the request over the network. note that such a request
		//   could still end up looping back to us
		if(destUri.scheme() == currentUri.scheme() && destUri.host() == currentUri.host() && destPort == currentPort)
		{
			// tell the proxy that we prefer the request to be handled
			//   internally, using the same route
			passthroughData["prefer-internal"] = true;
		}

		// these fields are needed in case proxy routing is not used
		if(adata.trusted)
			passthroughData["trusted"] = true;

		// share requests to the same URI
		passthroughData["auto-share"] = autoShare;

		outReq->setPassthroughData(passthroughData);
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

		prepareOutReq(nextUri, true);

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

			if(state == Proxying)
			{
				if(outReq->bytesAvailable() > 0)
				{
					// stop keep alive timer only if we have to send data. if the
					//   response body is empty, then the timer is left alone
					timer->stop();

					int avail = req->writeBytesAvailable();
					if(avail <= 0)
						return;

					QByteArray buf = outReq->readBody(avail);

					if(responseFilters)
					{
						buf = responseFilters->update(buf);
						if(buf.isNull())
						{
							logRequestError(outReq->requestMethod(), outReq->requestUri(), outReq->requestHeaders());

							errorMessage = QString("filter error: %1").arg(responseFilters->errorMessage());
							doError();
							return;
						}
					}

					writeBody(buf);

					sentOutReqData += buf.size();
				}

				if(outReq->bytesAvailable() == 0 && outReq->isFinished())
				{
					if(responseFilters)
					{
						QByteArray buf = responseFilters->finalize();
						if(buf.isNull())
						{
							logRequestError(outReq->requestMethod(), outReq->requestUri(), outReq->requestHeaders());

							errorMessage = QString("filter error: %1").arg(responseFilters->errorMessage());
							doError();
							return;
						}

						delete responseFilters;
						responseFilters = 0;

						if(!buf.isEmpty())
						{
							writeBody(buf);

							sentOutReqData += buf.size();
						}
					}

					HttpResponseData responseData;
					responseData.code = outReq->responseCode();
					responseData.reason = outReq->responseReason();
					responseData.headers = outReq->responseHeaders();

					logRequest(outReq->requestMethod(), outReq->requestUri(), outReq->requestHeaders(), responseData.code, sentOutReqData);

					retries = 0;

					cleanupOutReq();

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

					if(instruct.channels.count() > connectionSubscriptionMax)
					{
						instruct.channels = instruct.channels.mid(0, connectionSubscriptionMax);

						auto routeInfo = LogUtil::RouteInfo(adata.route, logLevel);
						LogUtil::logForRoute(routeInfo, "httpsession: too many subscriptions");
					}

					if(instruct.holdMode == Instruct::StreamHold)
					{
						if(instruct.keepAliveTimeout < 0)
							timer->stop();

						prepareToSendQueueOrHold();
					}
				}
			}
			else
			{
				// unexpected state
				assert(0);
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

	void logRequest(const QString &method, const QUrl &uri, const HttpHeaders &headers, int code, int bodySize)
	{
		LogUtil::RequestData rd;

		// only log route id if explicitly set
		if(!adata.statsRoute.isEmpty())
			rd.routeId = adata.route;

		rd.status = LogUtil::Response;

		rd.requestData.method = method;
		rd.requestData.uri = uri;
		rd.requestData.headers = headers;

		rd.responseData.code = code;
		rd.responseBodySize = bodySize;

		LogUtil::logRequest(LOG_LEVEL_INFO, rd, logConfig);
	}

	void logRequestError(const QString &method, const QUrl &uri, const HttpHeaders &headers)
	{
		LogUtil::RequestData rd;

		// only log route id if explicitly set
		if(!adata.statsRoute.isEmpty())
			rd.routeId = adata.route;

		rd.status = LogUtil::Error;

		rd.requestData.method = method;
		rd.requestData.uri = uri;
		rd.requestData.headers = headers;

		LogUtil::logRequest(LOG_LEVEL_INFO, rd, logConfig);
	}

	void incCounter(Stats::Counter c, int count = 1)
	{
		stats->incCounter(adata.statsRoute.toUtf8(), c, count);
	}

	void writeBody(const QByteArray &body)
	{
		incCounter(Stats::ClientContentBytesSent, body.size());

		req->writeBody(body);
	}

private slots:
	void doError()
	{
		if(instruct.holdMode == Instruct::ResponseHold)
		{
			QByteArray msg;

			if(adata.debug)
				msg = errorMessage.toUtf8();
			else
				msg = "Error while proxying to origin.";

			HttpHeaders headers;
			headers += HttpHeader("Content-Type", "text/plain");
			respond(502, "Bad Gateway", headers, msg + '\n');
		}
		else // StreamHold, NoHold
		{
			prepareToClose();

			if(adata.debug)
				writeBody("\n\n" + errorMessage.toUtf8() + '\n');

			req->endBody();
		}
	}

	void req_bytesWritten(int count)
	{
		Q_UNUSED(count);

		if(req->isFinished())
		{
			doFinish();
			return;
		}
	}

	void req_writeBytesChanged()
	{
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

	void req_paused()
	{
		doFinish(true); // finish for retry
	}

	void outReq_readyRead()
	{
		if(!haveOutReqHeaders)
		{
			haveOutReqHeaders = true;

			if(state == Proxying)
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

				responseFilters = new FilterStack(fc, allFilters);
			}
		}

		tryProcessOutReq();
	}

	void outReq_error()
	{
		logRequestError(outReq->requestMethod(), outReq->requestUri(), outReq->requestHeaders());

		cleanupOutReq();

		if(state == Proxying)
		{
			log_debug("httpsession: failed to retrieve next link");

			// can't retry if we started sending data

			if(sentOutReqData <= 0 && retries < RETRY_MAX)
			{
				int delay = RETRY_TIMEOUT;
				for(int n = 0; n < retries; ++n)
					delay *= 2;
				delay += QRandomGenerator::global()->generate() % RETRY_RAND_MAX;

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
		else
		{
			// unexpected state
			assert(0);
		}
	}

private:
	void timer_timeout()
	{
		if(instruct.holdMode == Instruct::ResponseHold)
		{
			// send timeout response
			respond(instruct.response.code, instruct.response.reason, instruct.response.headers, instruct.response.body);
		}
		else if(instruct.holdMode == Instruct::StreamHold)
		{
			writeBody(instruct.keepAliveData);

			setupKeepAlive();

			stats->addActivity(adata.statsRoute.toUtf8(), 1);
		}
	}

	void retryTimer_timeout()
	{
		requestNextLink();
	}
};

HttpSession::HttpSession(ZhttpRequest *req, const HttpSession::AcceptData &adata, const Instruct &instruct, ZhttpManager *zhttpOut, StatsManager *stats, RateLimiter *updateLimiter, PublishLastIds *publishLastIds, HttpSessionUpdateManager *updateManager, int connectionSubscriptionMax, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, req, adata, instruct, zhttpOut, stats, updateLimiter, publishLastIds, updateManager, connectionSubscriptionMax);
}

HttpSession::~HttpSession()
{
	delete d;
}

Instruct::HoldMode HttpSession::holdMode() const
{
	if(d->instruct.holdMode == Instruct::NoHold)
	{
		// NoHold is a temporary internal state for stream
		return Instruct::StreamHold;
	}
	else
		return d->instruct.holdMode;
}

ZhttpRequest::Rid HttpSession::rid() const
{
	return d->req->rid();
}

QUrl HttpSession::requestUri() const
{
	return d->req->requestUri();
}

bool HttpSession::isRetry() const
{
	return d->adata.isRetry;
}

QString HttpSession::statsRoute() const
{
	return d->adata.statsRoute;
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

QByteArray HttpSession::retryToAddress() const
{
	return d->retryToAddress;
}

RetryRequestPacket HttpSession::retryPacket() const
{
	return d->retryPacket;
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

Callback<std::tuple<HttpSession *, const QString &>> & HttpSession::subscribeCallback()
{
	return d->subscribeCallback;
}

Callback<std::tuple<HttpSession *, const QString &>> & HttpSession::unsubscribeCallback()
{
	return d->unsubscribeCallback;
}

Callback<std::tuple<HttpSession *>> & HttpSession::finishedCallback()
{
	return d->finishedCallback;
}

#include "httpsession.moc"
