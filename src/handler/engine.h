/*
 * Copyright (C) 2015-2022 Fanout, Inc.
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

class Engine : public QObject
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
		QString inspectSpec;
		QString acceptSpec;
		QString retryOutSpec;
		QString wsControlInSpec;
		QString wsControlOutSpec;
		QString statsSpec;
		QString commandSpec;
		QString stateSpec;
		QString proxyStatsSpec;
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
			statsConnectionTtl(-1),
			statsSubscriptionTtl(-1),
			statsReportInterval(-1)
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
