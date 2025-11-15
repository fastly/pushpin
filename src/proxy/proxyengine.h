/*
 * Copyright (C) 2012-2023 Fanout, Inc.
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

#ifndef PROXYENGINE_H
#define PROXYENGINE_H

#include <map>
#include <QStringList>
#include <QHostAddress>
#include <boost/signals2.hpp>
#include "jwt.h"
#include "xffrule.h"

// Each session can have a bunch of timers:
// 2 per incoming zhttprequest/zwebsocket
// 2 per outgoing zhttprequest/zwebsocket
// 1 per wsproxysession
// 2 per websocketoverhttp
// 1 per inspect/accept request
#define TIMERS_PER_SESSION 10

// Each zroute has a zhttpmanager, which has up to 8 timers
#define TIMERS_PER_ZROUTE 10

// Each zroute has a zhttpmanager, which has up to 8 socket notifiers
#define SOCKETNOTIFIERS_PER_ZROUTE 10

#define PROMETHEUS_CONNECTIONS_MAX 16
#define ZROUTES_MAX 100

using std::map;
using Connection = boost::signals2::scoped_connection;

class StatsManager;
class DomainMap;

/// Orchestrates the core proxy service:
/// - Accepts incoming HTTP/WebSocket connections
/// - Routes requests with inspection/accept handlers
/// - Creates and manages proxy sessions for backend connections
/// - Processes retry requests
/// - Coordinates statistics tracking
class Engine
{
public:
	class Configuration
	{
	public:
		int id;
		QString appVersion;
		QByteArray clientId;
		QStringList serverInSpecs;
		QStringList serverInStreamSpecs;
		QStringList serverOutSpecs;
		QStringList clientOutSpecs;
		QStringList clientOutStreamSpecs;
		QStringList clientInSpecs;
		QString inspectSpec;
		QString acceptSpec;
		QString retryInSpec;
		QStringList wsControlInitSpecs;
		QStringList wsControlStreamSpecs;
		QString statsSpec;
		QString commandSpec;
		QStringList intServerInSpecs;
		QStringList intServerInStreamSpecs;
		QStringList intServerOutSpecs;
		int ipcFileMode;
		int sessionsMax;
		int inspectTimeout;
		int inspectPrefetch;
		bool debug;
		bool autoCrossOrigin;
		bool acceptXForwardedProto;
		bool setXForwardedProto;
		bool setXForwardedProtocol;
		XffRule xffUntrustedRule;
		XffRule xffTrustedRule;
		QList<QByteArray> origHeadersNeedMark;
		bool acceptPushpinRoute;
		QByteArray cdnLoop;
		bool logFrom;
		bool logUserAgent;
		QByteArray sigIss;
		Jwt::EncodingKey sigKey;
		Jwt::DecodingKey upstreamKey;
		QString sockJsUrl;
		bool statsConnectionSend;
		int statsConnectionTtl;
		int statsConnectionsMaxTtl;
		int statsReportInterval;
		QString prometheusPort;
		QString prometheusPrefix;

		Configuration() :
			id(0),
			ipcFileMode(-1),
			sessionsMax(-1),
			inspectTimeout(8000),
			inspectPrefetch(10000),
			debug(false),
			autoCrossOrigin(false),
			acceptXForwardedProto(false),
			setXForwardedProto(false),
			setXForwardedProtocol(false),
			acceptPushpinRoute(false),
			logFrom(false),
			logUserAgent(false),
			statsConnectionSend(false),
			statsConnectionTtl(-1),
			statsConnectionsMaxTtl(-1),
			statsReportInterval(-1)
		{
		}
	};

	Engine(DomainMap *domainMap);
	~Engine();

	StatsManager *statsManager() const;

	bool start(const Configuration &config);
	void routesChanged();

private:
	class Private;
	Private *d;
};

#endif
