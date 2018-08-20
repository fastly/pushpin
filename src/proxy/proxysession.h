/*
 * Copyright (C) 2012-2017 Fanout, Inc.
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

#ifndef PROXYSESSION_H
#define PROXYSESSION_H

#include <QObject>
#include "logutil.h"
#include "domainmap.h"

class InspectData;
class AcceptData;
class ZrpcManager;
class ZRoutes;
class XffRule;
class RequestSession;

class ProxySession : public QObject
{
	Q_OBJECT

public:
	ProxySession(ZRoutes *zroutes, ZrpcManager *acceptManager, const LogUtil::Config &logConfig, QObject *parent = 0);
	~ProxySession();

	void setRoute(const DomainMap::Entry &route);
	void setDefaultSigKey(const QByteArray &iss, const QByteArray &key);
	void setAcceptXForwardedProtocol(bool enabled);
	void setUseXForwardedProtocol(bool protoEnabled, bool protocolEnabled);
	void setXffRules(const XffRule &untrusted, const XffRule &trusted);
	void setOrigHeadersNeedMark(const QList<QByteArray> &names);
	void setProxyInitialResponseEnabled(bool enabled);

	void setInspectData(const InspectData &idata);

	// takes ownership
	void add(RequestSession *rs);

signals:
	void addNotAllowed(); // no more sharing, for whatever reason
	void finished();
	void requestSessionDestroyed(RequestSession *rs, bool accept);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
