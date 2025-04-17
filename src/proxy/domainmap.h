/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#ifndef DOMAINMAP_H
#define DOMAINMAP_H

#include <QPair>
#include <QString>
#include <QStringList>
#include "log.h"
#include "httpheaders.h"
#include "jwt.h"
#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;
using Connection = boost::signals2::scoped_connection;

// this class offers fast access to the routes file. the table is maintained
//   by a background thread so that file access doesn't cause blocking.

class DomainMap
{
public:
	class JsonpConfig
	{
	public:
		enum Mode
		{
			Basic,
			Extended
		};

		Mode mode;
		QByteArray callbackParam;
		QByteArray bodyParam;
		QByteArray defaultCallback;
		QString defaultMethod;

		JsonpConfig() :
			mode(Extended)
		{
		}
	};

	enum Protocol
	{
		Http,
		WebSocket
	};

	class ZhttpRoute
	{
	public:
		QString baseSpec;
		bool req;
		int ipcFileMode;

		ZhttpRoute() :
			req(false),
			ipcFileMode(-1)
		{
		}

		bool isNull() const
		{
			return baseSpec.isEmpty();
		}

		bool operator==(const ZhttpRoute &other) const
		{
			// only compare spec
			return (baseSpec == other.baseSpec);
		}
	};

	class Target
	{
	public:
		enum Type
		{
			Default,
			Custom,
			Test
		};

		Type type;
		QString connectHost;
		int connectPort;
		ZhttpRoute zhttpRoute;
		bool ssl; // use https
		bool trusted; // bypass zurl access policies
		bool trustConnectHost; // verify cert against target host
		bool insecure; // ignore server certificate validity
		QString host; // override input host
		QStringList subscriptions; // implicit subscriptions
		bool overHttp; // use websocket-over-http protocol
		bool oneEvent; // send one event at a time with overHttp

		Target() :
			type(Default),
			connectPort(-1),
			ssl(false),
			trusted(false),
			trustConnectHost(false),
			insecure(false),
			overHttp(false),
			oneEvent(false)
		{
		}
	};

	class Entry
	{
	public:
		QByteArray id;
		QByteArray pathBeg;
		QByteArray sigIss;
		Jwt::EncodingKey sigKey;
		QByteArray prefix;
		bool origHeaders;
		QString asHost;
		int pathRemove;
		QByteArray pathPrepend;
		bool debug;
		bool autoCrossOrigin;
		JsonpConfig jsonpConfig;
		bool session;
		QByteArray sockJsPath;
		QByteArray sockJsAsPath;
		HttpHeaders headers;
		bool separateStats;
		bool grip;
		QList<Target> targets;
		int logLevel;

		bool isNull() const
		{
			return targets.isEmpty();
		}

		QByteArray statsRoute() const
		{
			if(separateStats)
				return id;
			else
				return QByteArray(); // global stats
		}

		Entry() :
			origHeaders(false),
			pathRemove(0),
			debug(false),
			autoCrossOrigin(false),
			session(false),
			separateStats(false),
			grip(true),
			logLevel(LOG_LEVEL_DEBUG)
		{
		}
	};

	DomainMap(bool newEventLoop);
	DomainMap(const QString &fileName, bool newEventLoop);
	~DomainMap();

	// shouldn't really ever need to call this, but it's here in case the
	//   underlying file watching doesn't work
	void reload();

	bool isIdShared(const QString &id) const;
	Entry entry(Protocol proto, bool ssl, const QString &domain, const QByteArray &path) const;
	Entry entry(const QString &id) const;

	QList<ZhttpRoute> zhttpRoutes() const;

	bool addRouteLine(const QString &line);

	Signal changed;

private:
	class Private;
	friend class Private;
	Private *d;

	class Thread;
	class Worker;
};

#endif
