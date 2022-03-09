/*
 * Copyright (C) 2012-2022 Fanout, Inc.
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

#ifndef ENGINE_H
#define ENGINE_H

#include <QObject>
#include <QStringList>
#include <QHostAddress>
#include "xffrule.h"

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
		bool logFrom;
		bool logUserAgent;
		QByteArray sigIss;
		QByteArray sigKey;
		QByteArray upstreamKey;
		QString sockJsUrl;
		QString updatesCheck;
		QString organizationName;
		bool quietCheck;
		int connectionsMax;
		int statsConnectionTtl;
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
			statsConnectionTtl(-1)
		{
		}
	};

	Engine(QObject *parent = 0);
	~Engine();

	bool start(const Configuration &config);
	void reload();

private:
	class Private;
	Private *d;
};

#endif
