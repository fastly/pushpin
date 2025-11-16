/*
 * Copyright (C) 2014-2022 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#ifndef WSPROXYSESSION_H
#define WSPROXYSESSION_H

#include "callback.h"
#include "logutil.h"
#include "domainmap.h"
#include <boost/signals2.hpp>

using std::map;
using Connection = boost::signals2::scoped_connection;

namespace Jwt {
	class EncodingKey;
	class DecodingKey;
}

class WebSocket;
class ZRoutes;
class WsControlManager;
class StatsManager;
class ConnectionManager;
class XffRule;

/// Proxies WebSocket requests to backends with GRIP streaming and request sharing
class WsProxySession
{
public:
	WsProxySession(ZRoutes *zroutes, ConnectionManager *connectionManager, const LogUtil::Config &logConfig, StatsManager *stats = 0, WsControlManager *wsControlManager = 0);
	~WsProxySession();

	QHostAddress logicalClientAddress() const;
	QByteArray statsRoute() const;
	QByteArray cid() const;

	WebSocket *inSocket() const;
	WebSocket *outSocket() const;

	void setDebugEnabled(bool enabled);
	void setDefaultSigKey(const QByteArray &iss, const Jwt::EncodingKey &key);
	void setDefaultUpstreamKey(const Jwt::DecodingKey &key);
	void setAcceptXForwardedProtocol(bool enabled);
	void setUseXForwardedProtocol(bool protoEnabled, bool protocolEnabled);
	void setXffRules(const XffRule &untrusted, const XffRule &trusted);
	void setOrigHeadersNeedMark(const QList<QByteArray> &names);
	void setAcceptPushpinRoute(bool enabled);
	void setCdnLoop(const QByteArray &value);

	// takes ownership
	void start(WebSocket *sock, const QByteArray &publicCid, const DomainMap::Entry &route);

	// NOTE: for performance reasons we use callbacks instead of signals/slots
	Callback<std::tuple<WsProxySession *>> & finishedByPassthroughCallback();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
