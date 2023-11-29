/*
 * Copyright (C) 2012-2023 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
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

#ifndef ENGINE_H
#define ENGINE_H

#include <QObject>
#include <QStringList>
#include <QHostAddress>
#include "jwt.h"
#include "xffrule.h"
#include <boost/signals2.hpp>

using Connection = boost::signals2::connection;

class StatsManager;

class Engine : public QObject
{
	Q_OBJECT

public:
	class Configuration
	{
	public:
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
		QString wsControlInSpec;
		QString wsControlOutSpec;
		QString statsSpec;
		QString commandSpec;
		QStringList intServerInSpecs;
		QStringList intServerInStreamSpecs;
		QStringList intServerOutSpecs;
		int ipcFileMode;
		int maxWorkers;
		int inspectTimeout;
		int inspectPrefetch;
		QString routesFile;
		QStringList routeLines;
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
		QString updatesCheck;
		QString organizationName;
		bool quietCheck;
		int connectionsMax;
		bool statsConnectionSend;
		int statsConnectionTtl;
		int statsConnectionsMaxTtl;
		int statsReportInterval;
		QString prometheusPort;
		QString prometheusPrefix;

		Configuration() :
			ipcFileMode(-1),
			maxWorkers(-1),
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
			updatesCheck("check"),
			quietCheck(false),
			connectionsMax(-1),
			statsConnectionSend(false),
			statsConnectionTtl(-1),
			statsConnectionsMaxTtl(-1),
			statsReportInterval(-1)
		{
		}
	};

	Engine(QObject *parent = 0);
	~Engine();

	StatsManager *statsManager() const;

	bool start(const Configuration &config);
	void reload();

private:
	class Private;
	Private *d;
};

#endif
