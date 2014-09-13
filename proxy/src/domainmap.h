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

#ifndef DOMAINMAP_H
#define DOMAINMAP_H

#include <QPair>
#include <QString>

// this class offers fast access to the routes file. the table is maintained
//   by a background thread so that file access doesn't cause blocking.

class DomainMap
{
public:
	enum Protocol
	{
		Http,
		WebSocket
	};

	class Target
	{
	public:
		QString connectHost;
		int connectPort;
		bool ssl; // use https
		bool trusted; // bypass zurl access policies
		bool insecure; // ignore server certificate validity
		QString host; // override input host
		QString subChannel; // force subscription for websocket test
		bool overHttp; // use websocket-over-http protocol

		Target() :
			connectPort(-1),
			ssl(false),
			trusted(false),
			insecure(false),
			overHttp(false)
		{
		}
	};

	class Entry
	{
	public:
		QByteArray id;
		QByteArray sigIss;
		QByteArray sigKey;
		QByteArray prefix;
		bool origHeaders;
		QString asHost;
		int pathRemove;
		QList<Target> targets;

		bool isNull() const
		{
			return targets.isEmpty();
		}

		Entry() :
			origHeaders(false),
			pathRemove(0)
		{
		}
	};

	DomainMap(const QString &fileName);
	~DomainMap();

	// shouldn't really ever need to call this, but it's here in case the
	//   underlying file watching doesn't work
	void reload();

	Entry entry(Protocol proto, bool ssl, const QString &domain, const QByteArray &path) const;

private:
	class Private;
	Private *d;

	class Thread;
	class Worker;
};

#endif
