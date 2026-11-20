/*
 * Copyright (C) 2017-2022 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include "logutil.h"

#include "log.h"
#include "qtcompat.h"
#include "tnetstring.h"
#include "variant.h"
#include <assert.h>
#include <stdarg.h>

#define MAX_DATA_LENGTH 1000
#define MAX_CONTENT_LENGTH 1000

namespace LogUtil {

static CowString trim(const CowString &in, int max) {
    if (in.length() > max && max >= 7)
        return in.mid(0, max / 2) + "..." + in.mid(in.length() - (max / 2) + 3);
    else
        return in;
}

static CowByteArray trim(const CowByteArray &in, int max) {
    if (in.size() > max && max >= 7)
        return in.mid(0, max / 2) + "..." + in.mid(in.size() - (max / 2) + 3);
    else
        return in;
}

static CowString makeLastIdsStr(const HttpHeaders &headers) {
    CowString out;

    bool first = true;
    foreach (const HttpHeaderParameters &params, headers.getAllAsParameters("Grip-Last")) {
        if (!first)
            out += ' ';
        out += CowString("#%1=%2").arg(CowString::fromUtf8(params[0].first),
                                       CowString::fromUtf8(params.get("last-id")));
        first = false;
    }

    return out;
}

static void logPacketWithOutputLevel(int outputLevel, int level, const CowString &message,
                                     const Variant &data = Variant(), int dataMax = -1,
                                     const CowByteArray &content = CowByteArray(),
                                     int contentMax = -1) {
    CowString out = message;

    if (data.isValid()) {
        out += CowString(" ") + trim(TnetString::variantToString(data, -1), dataMax);
    }

    if (!content.isNull()) {
        out += CowString(" ") + CowString::number((int)content.size()) + " ";
        CowByteArray buf = trim(content, contentMax);
        out += TnetString::variantToString(Variant(buf.asQByteArray()), -1);
    }

    logWithOutputLevel(outputLevel, level, "%s", cowPrintable(out));
}

static void logPacket(int level, const CowString &message, const Variant &data = Variant(),
                      int dataMax = -1, const CowByteArray &content = CowByteArray(),
                      int contentMax = -1) {
    logPacketWithOutputLevel(-1, level, message, data, dataMax, content, contentMax);
}

static void logPacket(int level, const Variant &data, const char *fmt, va_list ap) {
    logPacket(level, CowString::vasprintf(fmt, ap), data, MAX_DATA_LENGTH);
}

static void logPacket(int level, const CowByteArray &content, const char *fmt, va_list ap) {
    logPacket(level, CowString::vasprintf(fmt, ap), Variant(), -1, content, MAX_CONTENT_LENGTH);
}

static void logPacket(int level, const Variant &data, const CowString &contentField,
                      const char *fmt, va_list ap) {
    Variant meta;
    CowByteArray content;

    if (typeId(data) == VariantType::Hash) {
        // Extract content. Meta is the remaining data
        VariantHash hdata = data.toHash();
        content = hdata.value(contentField.asQString()).toByteArray();
        hdata.remove(contentField.asQString());
        meta = hdata;
    } else {
        // If data isn't a hash, then we can't extract content, so the meta part will be the
        // entire data
        meta = data;
    }

    logPacket(level, CowString::vasprintf(fmt, ap), meta, MAX_DATA_LENGTH, content,
              MAX_CONTENT_LENGTH);
}

void logVariant(int level, const Variant &data, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logPacket(level, data, fmt, ap);
    va_end(ap);
}

void logByteArray(int level, const CowByteArray &content, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logPacket(level, content, fmt, ap);
    va_end(ap);
}

void logVariantWithContent(int level, const Variant &data, const CowString &contentField,
                           const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logPacket(level, data, contentField, fmt, ap);
    va_end(ap);
}

void logRequest(int level, const RequestData &data, const Config &config) {
    CowString msg = CowString("%1 %2").arg(data.requestData.method,
                                           data.requestData.uri.toString(CowUrl::FullyEncoded));

    if (!data.targetStr.isEmpty())
        msg += CowString(" -> %1").arg(data.targetStr);

    if (data.requestData.uri.scheme() != "http" && data.requestData.uri.scheme() != "https" &&
        data.targetOverHttp)
        msg += "[http]";

    if (config.fromAddress && !data.fromAddress.isNull())
        msg += CowString(" from=%1").arg(data.fromAddress.toString());

    CowUrl ref = CowUrl(CowString::fromUtf8(data.requestData.headers.get("Referer")));
    if (!ref.isEmpty())
        msg += CowString(" ref=%1").arg(ref.toString(CowUrl::FullyEncoded));

    if (!data.routeId.isEmpty())
        msg += CowString(" route=%1").arg(data.routeId);

    if (data.status == LogUtil::Response) {
        msg += CowString(" code=%1 %2")
                   .arg(CowString::number(data.responseData.code),
                        CowString::number(data.responseBodySize));
    } else if (data.status == LogUtil::Accept) {
        msg += " accept";
    } else {
        msg += " error";
    }

    if (data.retry)
        msg += " retry";

    if (data.sharedBy)
        msg += CowString::asprintf(" shared=%p", data.sharedBy);

    if (config.userAgent) {
        CowString userAgent = CowString::fromUtf8(data.requestData.headers.get("User-Agent"));
        if (!userAgent.isEmpty())
            msg += CowString(" ua=%1").arg(userAgent);
    }

    CowString lastIdsStr = makeLastIdsStr(data.requestData.headers);
    if (!lastIdsStr.isEmpty())
        msg += CowString(" ") + lastIdsStr;

    logWithOutputLevel(data.logLevel, level, "%s", cowPrintable(msg));
}

void logForRoute(int level, const RouteInfo &routeInfo, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    CowString msg = CowString::vasprintf(fmt, ap);
    if (!routeInfo.id.isEmpty())
        msg += CowString(" route=%1").arg(routeInfo.id);
    logPacketWithOutputLevel(routeInfo.logLevel, level, msg);
    va_end(ap);
}

} // namespace LogUtil
