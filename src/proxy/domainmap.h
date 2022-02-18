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

#ifndef DOMAINMAP_H
#define DOMAINMAP_H

#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include "httpheaders.h"

// this class offers fast access to the routes file. the table is maintained
//   by a background thread so that file access doesn't cause blocking.

class DomainMap : public QObject
{
	Q_OBJECT

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
		QByteArray sigKey;
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
			grip(true)
		{
		}
	};

	DomainMap(QObject *parent = 0);
	DomainMap(const QString &fileName, QObject *parent = 0);
	~DomainMap();

	// shouldn't really ever need to call this, but it's here in case the
	//   underlying file watching doesn't work
	void reload();

	bool isIdShared(const QString &id) const;
	Entry entry(Protocol proto, bool ssl, const QString &domain, const QByteArray &path) const;
	Entry entry(const QString &id) const;

	QList<ZhttpRoute> zhttpRoutes() const;

	bool addRouteLine(const QString &line);

signals:
	void changed();

private:
	class Private;
	friend class Private;
	Private *d;

	class Thread;
	class Worker;
};

#endif
