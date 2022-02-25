/*
 * Copyright (C) 2016-2022 Fanout, Inc.
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

#include "httpsession.h"

#include <assert.h>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
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
		if(vbody.isValid() && (vbody.type() == QVariant::Map || vbody.type() == QVariant::List))
		{
			QJsonDocument doc;
			if(vbody.type() == QVariant::Map)
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
	QHash<QString, Instruct::Channel> channels;
	RTimer *timer;
	RTimer *retryTimer;
	StatsManager *stats;
	ZhttpManager *outZhttp;
	ZhttpRequest *outReq; // for fetching next links
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
	RetryRequestPacket retryPacket;
	LogUtil::Config logConfig;
	FilterStack *responseFilters;
	QSet<QString> activeChannels;
	int connectionSubscriptionMax;
	SubscribeFunc subscribeCallback;
	void *subscribeData;
	UnsubscribeFunc unsubscribeCallback;
	void *unsubscribeData;
	FinishedFunc finishedCallback;
	void *finishedData;

	Private(HttpSession *_q, ZhttpRequest *_req, const HttpSession::AcceptData &_adata, const Instruct &_instruct, ZhttpManager *_outZhttp, StatsManager *_stats, RateLimiter *_updateLimiter, PublishLastIds *_publishLastIds, HttpSessionUpdateManager *_updateManager, int _connectionSubscriptionMax) :
		QObject(_q),
		q(_q),
		req(_req),
		stats(_stats),
		outZhttp(_outZhttp),
		outReq(0),
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
		subscribeCallback(0),
		subscribeData(0),
		unsubscribeCallback(0),
		unsubscribeData(0),
		finishedCallback(0),
		finishedData(0)
	{
		state = NotStarted;

		req->setParent(this);
		connect(req, &ZhttpRequest::bytesWritten, this, &Private::req_bytesWritten);
		connect(req, &ZhttpRequest::error, this, &Private::req_error);

		timer = new RTimer(this);
		connect(timer, &RTimer::timeout, this, &Private::timer_timeout);

		retryTimer = new RTimer(this);
		connect(retryTimer, &RTimer::timeout, this, &Private::retryTimer_timeout);
		retryTimer->setSingleShot(true);

		adata = _adata;
		instruct = _instruct;

		currentUri = req->requestUri();

		if(!instruct.nextLink.isEmpty())
			nextUri = currentUri.resolved(instruct.nextLink);
	}

	~Private()
	{
		cleanup();

		updateManager->unregisterSession(q);

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
		stats->addConnection(rid.first + ':' + rid.second, adata.statsRoute.toUtf8(), StatsManager::Http, adata.logicalPeerAddress, req->requestUri().scheme() == "https", true);

		// set up implicit channels
		QPointer<QObject> self = this;
		foreach(const QString &name, adata.implicitChannels)
		{
			if(!channels.contains(name))
			{
				Instruct::Channel c;
				c.name = name;

				channels.insert(name, c);

				if(subscribeCallback)
				{
					subscribeCallback(subscribeData, q, name);
				}

				assert(self); // deleting here would leak subscriptions/connections
			}
		}

		if(instruct.channels.count() > connectionSubscriptionMax)
		{
			instruct.channels = instruct.channels.mid(0, connectionSubscriptionMax);
			log_warning("httpsession: too many subscriptions");
		}

		// need to send initial content?
		if((instruct.holdMode == Instruct::NoHold || instruct.holdMode == Instruct::StreamHold) && !adata.responseSent)
		{
			// send initial response
			HttpHeaders headers = instruct.response.headers;
			headers.removeAll("Content-Length");
			if(adata.autoCrossOrigin)
				Cors::applyCorsHeaders(req->requestHeaders(), &headers);
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
		cleanupAction();

		delete outReq;
		outReq = 0;

		delete responseFilters;
		responseFilters = 0;
	}

	void cleanupAction()
	{
		if(pendingAction)
		{
			pendingAction->sessions.remove(q);
			pendingAction = 0;
		}
	}

	void setupKeepAlive()
	{
		if(instruct.keepAliveTimeout >= 0)
		{
			int timeout = instruct.keepAliveTimeout * 1000;
			timeout = qMax(timeout - (qrand() % KEEPALIVE_RAND_MAX), 0);
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

		publishQueue.clear();
		timer->stop();
		updateManager->unregisterSession(q);
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

			connect(req, &ZhttpRequest::paused, this, &Private::req_paused);
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
			if(unsubscribeCallback)
			{
				unsubscribeCallback(unsubscribeData, q, channel);
			}

			assert(self); // deleting here would leak subscriptions/connections
		}

		foreach(const QString &channel, channelsAdded)
		{
			if(subscribeCallback)
			{
				subscribeCallback(subscribeData, q, channel);
			}

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

				req->writeBody(body);

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

	void doFinish(bool retry = false)
	{
		ZhttpRequest::Rid rid = req->rid();

		QByteArray cid = rid.first + ':' + rid.second;

		log_debug("httpsession: cleaning up %s", cid.data());

		cleanup();

		QPointer<QObject> self = this;

		QHashIterator<QString, Instruct::Channel> it(channels);
		while(it.hasNext())
		{
			it.next();
			const QString &channel = it.key();

			if(unsubscribeCallback)
			{
				unsubscribeCallback(unsubscribeData, q, channel);
			}

			assert(self); // deleting here would leak subscriptions/connections
		}

		if(retry)
		{
			// refresh before remove, to ensure transition
			stats->refreshConnection(cid);
			stats->removeConnection(cid, true);

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

			retryPacket = rp;
		}
		else
		{
			stats->removeConnection(cid, false);
		}

		if(finishedCallback)
		{
			finishedCallback(finishedData, q);
		}
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
			// tell the proxy that we prefer the request to be handled
			//   internally, using the same route
			passthroughData["route"] = adata.route.toUtf8();
		}

		// these fields are needed in case proxy routing is not used
		if(!adata.sigIss.isEmpty())
			passthroughData["sig-iss"] = adata.sigIss;
		if(!adata.sigKey.isEmpty())
			passthroughData["sig-key"] = adata.sigKey;
		if(adata.trusted)
			passthroughData["trusted"] = true;

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

				req->writeBody(buf);

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
						req->writeBody(buf);

						sentOutReqData += buf.size();
					}
				}

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

				if(instruct.channels.count() > connectionSubscriptionMax)
				{
					instruct.channels = instruct.channels.mid(0, connectionSubscriptionMax);
					log_warning("httpsession: too many subscriptions");
				}

				if(instruct.holdMode == Instruct::StreamHold)
				{
					if(instruct.keepAliveTimeout < 0)
						timer->stop();

					prepareToSendQueueOrHold();
				}
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

		if(!adata.route.isEmpty())
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

		if(!adata.route.isEmpty())
			rd.routeId = adata.route;

		rd.status = LogUtil::Error;

		rd.requestData.method = method;
		rd.requestData.uri = uri;
		rd.requestData.headers = headers;

		LogUtil::logRequest(LOG_LEVEL_INFO, rd, logConfig);
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
				req->writeBody("\n\n" + errorMessage.toUtf8() + '\n');

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

		tryProcessOutReq();
	}

	void outReq_error()
	{
		logRequestError(outReq->requestMethod(), outReq->requestUri(), outReq->requestHeaders());

		delete responseFilters;
		responseFilters = 0;

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
		if(instruct.holdMode == Instruct::ResponseHold)
		{
			// send timeout response
			respond(instruct.response.code, instruct.response.reason, instruct.response.headers, instruct.response.body);
		}
		else if(instruct.holdMode == Instruct::StreamHold)
		{
			req->writeBody(instruct.keepAliveData);

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

void HttpSession::setSubscribeCallback(SubscribeFunc cb, void *data)
{
	d->subscribeCallback = cb;
	d->subscribeData = data;
}

void HttpSession::setUnsubscribeCallback(UnsubscribeFunc cb, void *data)
{
	d->unsubscribeCallback = cb;
	d->unsubscribeData = data;
}

void HttpSession::setFinishedCallback(FinishedFunc cb, void *data)
{
	d->finishedCallback = cb;
	d->finishedData = data;
}

#include "httpsession.moc"
