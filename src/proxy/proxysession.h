/*
 * Copyright (C) 2012-2022 Fanout, Inc.
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

#ifndef PROXYSESSION_H
#define PROXYSESSION_H

#include <QObject>
#include "logutil.h"
#include "domainmap.h"

namespace Jwt {
	class EncodingKey;
}

class InspectData;
class AcceptData;
class ZrpcManager;
class ZRoutes;
class StatsManager;
class XffRule;
class RequestSession;
#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;
using Connection = boost::signals2::scoped_connection;

class ProxySession : public QObject
{
	Q_OBJECT

public:
	ProxySession(ZRoutes *zroutes, ZrpcManager *acceptManager, const LogUtil::Config &logConfig, StatsManager *stats = 0, QObject *parent = 0);
	~ProxySession();

	void setRoute(const DomainMap::Entry &route);
	void setDefaultSigKey(const QByteArray &iss, const Jwt::EncodingKey &key);
	void setAcceptXForwardedProtocol(bool enabled);
	void setUseXForwardedProtocol(bool protoEnabled, bool protocolEnabled);
	void setXffRules(const XffRule &untrusted, const XffRule &trusted);
	void setOrigHeadersNeedMark(const QList<QByteArray> &names);
	void setAcceptPushpinRoute(bool enabled);
	void setCdnLoop(const QByteArray &value);
	void setProxyInitialResponseEnabled(bool enabled);

	void setInspectData(const InspectData &idata);

	// takes ownership
	void add(RequestSession *rs);

	Signal addNotAllowed; // no more sharing, for whatever reason
	Signal finished;
	boost::signals2::signal<void(RequestSession*, bool)> requestSessionDestroyed;

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
