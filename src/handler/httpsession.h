/*
 * Copyright (C) 2016-2023 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#ifndef HTTPSESSION_H
#define HTTPSESSION_H

#include <QObject>
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "callback.h"
#include "inspectdata.h"
#include "zhttprequest.h"
#include "instruct.h"
#include <boost/signals2.hpp>

using Connection = boost::signals2::scoped_connection;

class QTimer;
class ZhttpManager;
class StatsManager;
class PublishItem;
class RateLimiter;
class PublishLastIds;
class HttpSessionUpdateManager;
class RetryRequestPacket;

class HttpSession;

class HttpSession : public QObject
{
	Q_OBJECT

public:
	class AcceptData
	{
	public:
		QByteArray from;
		QHostAddress logicalPeerAddress;
		bool debug;
		bool isRetry;
		bool autoCrossOrigin;
		QByteArray jsonpCallback;
		bool jsonpExtendedResponse;
		int unreportedTime;
		HttpRequestData requestData;
		QString route;
		QString statsRoute;
		QString channelPrefix;
		int logLevel;
		QSet<QString> implicitChannels;
		bool trusted;
		bool responseSent;
		QString sid;
		bool haveInspectInfo;
		InspectData inspectInfo;

		AcceptData() :
			debug(false),
			isRetry(false),
			autoCrossOrigin(false),
			jsonpExtendedResponse(false),
			unreportedTime(-1),
			logLevel(-1),
			trusted(false),
			responseSent(false),
			haveInspectInfo(false)
		{
		}
	};

	HttpSession(ZhttpRequest *req, const HttpSession::AcceptData &adata, const Instruct &instruct, ZhttpManager *outZhttp, StatsManager *stats, RateLimiter *updateLimiter, PublishLastIds *publishLastIds, HttpSessionUpdateManager *updateManager, int connectionSubscriptionMax, QObject *parent = 0);
	~HttpSession();

	Instruct::HoldMode holdMode() const;
	ZhttpRequest::Rid rid() const;
	QUrl requestUri() const;
	bool isRetry() const;
	QString statsRoute() const;
	QString sid() const;
	QHash<QString, Instruct::Channel> channels() const;
	QHash<QString, QString> meta() const;
	QByteArray retryToAddress() const;
	RetryRequestPacket retryPacket() const;

	void start();
	void update();
	void publish(const PublishItem &item, const QList<QByteArray> &exposeHeaders = QList<QByteArray>());

	// NOTE: for performance reasons we use callbacks instead of signals/slots
	Callback<std::tuple<HttpSession *, const QString &>> & subscribeCallback();
	Callback<std::tuple<HttpSession *, const QString &>> & unsubscribeCallback();
	Callback<std::tuple<HttpSession *>> & finishedCallback();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
