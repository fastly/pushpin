/*
 * Copyright (C) 2015-2023 Fanout, Inc.
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

#ifndef HANDLERENGINE_H
#define HANDLERENGINE_H

#include <QObject>
#include <QStringList>
#include <QHostAddress>
#include <boost/signals2.hpp>
#include <map>

using std::map;
using Signal = boost::signals2::signal<void()>;
using Connection = boost::signals2::scoped_connection;

class HandlerEngine : public QObject
{
	Q_OBJECT

public:
	class Configuration
	{
	public:
		QString appVersion;
		QByteArray instanceId;
		QStringList serverInStreamSpecs;
		QStringList serverOutSpecs;
		QStringList clientOutSpecs;
		QStringList clientOutStreamSpecs;
		QStringList clientInSpecs;
		QStringList inspectSpecs;
		QStringList acceptSpecs;
		QString retryOutSpec;
		QString wsControlInSpec;
		QString wsControlOutSpec;
		QString statsSpec;
		QString commandSpec;
		QString stateSpec;
		QStringList proxyStatsSpecs;
		QString proxyCommandSpec;
		QString pushInSpec;
		QStringList pushInSubSpecs;
		bool pushInSubConnect;
		QHostAddress pushInHttpAddr;
		int pushInHttpPort;
		int pushInHttpMaxHeadersSize;
		int pushInHttpMaxBodySize;
		int ipcFileMode;
		bool shareAll;
		int messageRate;
		int messageHwm;
		int messageBlockSize;
		int messageWait;
		int idCacheTtl;
		int connectionsMax;
		int connectionSubscriptionMax;
		int subscriptionLinger;
		bool statsConnectionSend;
		int statsConnectionTtl;
		int statsSubscriptionTtl;
		int statsReportInterval;
		QString statsFormat;
		QString prometheusPort;
		QString prometheusPrefix;

		Configuration() :
			pushInSubConnect(false),
			pushInHttpPort(-1),
			pushInHttpMaxHeadersSize(-1),
			pushInHttpMaxBodySize(-1),
			ipcFileMode(-1),
			shareAll(false),
			messageRate(-1),
			messageHwm(-1),
			messageBlockSize(-1),
			messageWait(-1),
			idCacheTtl(-1),
			connectionsMax(-1),
			connectionSubscriptionMax(-1),
			subscriptionLinger(-1),
			statsConnectionSend(false),
			statsConnectionTtl(-1),
			statsSubscriptionTtl(-1),
			statsReportInterval(-1)
		{
		}
	};

	HandlerEngine(QObject *parent = 0);
	~HandlerEngine();

	bool start(const Configuration &config);
	void reload();

private:
	class Private;
	Private *d;
};

#endif
