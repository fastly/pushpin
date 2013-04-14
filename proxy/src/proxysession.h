/*
 * Copyright (C) 2012-2013 Fan Out Networks, Inc.
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

#ifndef PROXYSESSION_H
#define PROXYSESSION_H

#include <QObject>

class InspectData;
class AcceptData;
class ZurlManager;
class DomainMap;
class RequestSession;

class ProxySession : public QObject
{
	Q_OBJECT

public:
	ProxySession(ZurlManager *zurlManager, DomainMap *domainMap, QObject *parent = 0);
	~ProxySession();

	void setDefaultSigKey(const QByteArray &iss, const QByteArray &key);
	void setDefaultUpstreamKey(const QByteArray &iss, const QByteArray &key);
	void setUseXForwardedProtocol(bool enabled);

	void setInspectData(const InspectData &idata);

	// takes ownership
	void add(RequestSession *rs);

	void cannotAccept();

signals:
	void addNotAllowed(); // no more sharing, for whatever reason
	void finishedByPassthrough();
	void finishedForAccept(const AcceptData &adata);
	void requestSessionDestroyed(RequestSession *rs);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
