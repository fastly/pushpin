/*
 * Copyright (C) 2014-2017 Fanout, Inc.
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

#ifndef PROXYUTIL_H
#define PROXYUTIL_H

#include <QByteArray>
#include <QList>
#include <QHostAddress>
#include "packet/httprequestdata.h"
#include "domainmap.h"
#include "xffrule.h"

class InspectData;

namespace ProxyUtil {

bool manipulateRequestHeaders(const char *logprefix, void *object, HttpRequestData *requestData, const QByteArray &defaultUpstreamKey, const DomainMap::Entry &entry, const QByteArray &sigIss, const QByteArray &sigKey, bool acceptXForwardedProtocol, bool useXForwardedProtocol, const XffRule &xffTrustedRule, const XffRule &xffRule, const QList<QByteArray> &origHeadersNeedMark, const QHostAddress &peerAddress, const InspectData &idata, bool stripHeaders);

void applyHost(QUrl *url, const QString &host);

void applyHostHeader(HttpHeaders *headers, const QUrl &uri);

QString targetToString(const DomainMap::Target &target);

}

#endif
