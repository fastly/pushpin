/*
 * Copyright (C) 2014-2022 Fanout, Inc.
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

#ifndef PROXYUTIL_H
#define PROXYUTIL_H

#include <QByteArray>
#include <QList>
#include <QHostAddress>
#include "packet/httprequestdata.h"
#include "domainmap.h"
#include "xffrule.h"

namespace Jwt {
    class EncodingKey;
    class DecodingKey;
}

class InspectData;

namespace ProxyUtil {

bool checkTrustedClient(const char *logprefix, void *object, const HttpRequestData &requestData, const Jwt::DecodingKey &defaultUpstreamKey);

void manipulateRequestHeaders(const char *logprefix, void *object, HttpRequestData *requestData, bool trustedClient, const DomainMap::Entry &entry, const QByteArray &sigIss, const Jwt::EncodingKey &sigKey, bool acceptXForwardedProtocol, bool useXForwardedProto, bool useXForwardedProtocol, const XffRule &xffTrustedRule, const XffRule &xffRule, const QList<QByteArray> &origHeadersNeedMark, bool acceptPushpinRoute, const QByteArray &cdnLoop, const QHostAddress &peerAddress, const InspectData &idata, bool gripEnabled, bool intReq);

void applyHost(QUrl *url, const QString &host);

void applyHostHeader(HttpHeaders *headers, const QUrl &uri);
void applyGripSig(const char *logprefix, void *object, HttpHeaders *headers, const QByteArray &sigIss, const Jwt::EncodingKey &sigKey);

QString targetToString(const DomainMap::Target &target);

QHostAddress getLogicalAddress(const HttpHeaders &headers, const XffRule &xffRule, const QHostAddress &peerAddress);

}

#endif
