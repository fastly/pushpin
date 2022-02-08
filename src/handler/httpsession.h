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

#ifndef HTTPSESSION_H
#define HTTPSESSION_H

#include <QObject>
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "inspectdata.h"
#include "zhttprequest.h"
#include "instruct.h"

class QTimer;
class ZhttpManager;
class StatsManager;
class PublishItem;
class RateLimiter;
class PublishLastIds;
class HttpSessionUpdateManager;
class RetryRequestPacket;

class HttpSession;

typedef void (*SubscribeFunc)(void *data, HttpSession *hs, const QString &channel);
typedef void (*UnsubscribeFunc)(void *data, HttpSession *hs, const QString &channel);
typedef void (*FinishedFunc)(void *data, HttpSession *hs);

class HttpSession : public QObject
{
	Q_OBJECT

public:
	class AcceptData
	{
	public:
		QHostAddress logicalPeerAddress;
		bool debug;
		bool isRetry;
		bool autoCrossOrigin;
		QByteArray jsonpCallback;
		bool jsonpExtendedResponse;
		HttpRequestData requestData;
		QString route;
		QString statsRoute;
		QString channelPrefix;
		QSet<QString> implicitChannels;
		QByteArray sigIss;
		QByteArray sigKey;
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
	RetryRequestPacket retryPacket() const;

	void start();
	void update();
	void publish(const PublishItem &item, const QList<QByteArray> &exposeHeaders = QList<QByteArray>());

	// NOTE: for performance reasons we use callbacks instead of signals/slots
	void setSubscribeCallback(SubscribeFunc cb, void *data);
	void setUnsubscribeCallback(UnsubscribeFunc cb, void *data);
	void setFinishedCallback(FinishedFunc cb, void *data);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
