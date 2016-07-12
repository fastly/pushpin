/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifndef ENGINE_H
#define ENGINE_H

#include <QObject>
#include <QStringList>
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
		int ipcFileMode;
		int maxWorkers;
		int inspectTimeout;
		int inspectPrefetch;
		QString routesFile;
		QStringList routeLines;
		bool debug;
		bool autoCrossOrigin;
		bool acceptXForwardedProtocol;
		bool useXForwardedProtocol;
		XffRule xffUntrustedRule;
		XffRule xffTrustedRule;
		QList<QByteArray> origHeadersNeedMark;
		QByteArray sigIss;
		QByteArray sigKey;
		QByteArray upstreamKey;
		QString sockJsUrl;
		bool updatesCheck;
		QString organizationName;

		Configuration() :
			ipcFileMode(-1),
			maxWorkers(-1),
			inspectTimeout(8000),
			inspectPrefetch(10000),
			debug(false),
			autoCrossOrigin(false),
			acceptXForwardedProtocol(false),
			useXForwardedProtocol(false),
			updatesCheck(true)
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
