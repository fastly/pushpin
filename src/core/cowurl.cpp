/*
 * Copyright (C) 2026 Fastly, Inc.
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
 */

#include "cowurl.h"

#include "urlquery.h"
#include <assert.h>

static QString dataToString(ffi::CowUrlData data) {
    assert(data.data);

    QString s = QString::fromUtf8(data.data, data.len);
    ffi::cow_url_data_delete(data);
    return s;
}

static QByteArray dataToByteArray(ffi::CowUrlData data) {
    assert(data.data);

    QByteArray ba(data.data, data.len);
    ffi::cow_url_data_delete(data);
    return ba;
}

CowUrl::CowUrl() : inner_(nullptr) {}

CowUrl::CowUrl(const QString &url) {
    QByteArray utf8 = url.toUtf8();
    inner_ = ffi::cow_url_from_string(utf8.constData());
}

CowUrl::CowUrl(const QString &url, [[maybe_unused]] ParsingMode mode) {
    QByteArray utf8 = url.toUtf8();
    inner_ = ffi::cow_url_from_string(utf8.constData());
}

CowUrl::CowUrl(const char *url) { inner_ = ffi::cow_url_from_string(url); }

CowUrl::CowUrl(const CowUrl &other) {
    inner_ = other.inner_ ? ffi::cow_url_clone(other.inner_) : nullptr;
}

CowUrl::~CowUrl() { ffi::cow_url_destroy(inner_); }

CowUrl &CowUrl::operator=(const CowUrl &other) {
    if (this != &other) {
        ffi::cow_url_destroy(inner_);
        inner_ = other.inner_ ? ffi::cow_url_clone(other.inner_) : nullptr;
    }

    return *this;
}

CowUrl CowUrl::fromEncoded(const QByteArray &input, [[maybe_unused]] ParsingMode mode) {
    CowUrl url;
    url.inner_ = ffi::cow_url_from_string(input.constData());
    return url;
}

QString CowUrl::fromPercentEncoding(const QByteArray &input) {
    return QString::fromUtf8(QByteArray::fromPercentEncoding(input));
}

QByteArray CowUrl::toPercentEncoding(const QString &input) {
    return input.toUtf8().toPercentEncoding();
}

bool CowUrl::isValidRelativeUrl(const QString &relativeUrl) {
    CowUrl dummyBase("http://example.com/");
    CowUrl resolved = dummyBase.resolved(relativeUrl);
    return resolved.isValid();
}

bool CowUrl::isEmpty() const { return inner_ == nullptr; }

bool CowUrl::isValid() const { return inner_ != nullptr; }

void CowUrl::clear() {
    ffi::cow_url_destroy(inner_);
    inner_ = nullptr;
}

void CowUrl::setScheme(const QString &scheme) {
    if (!inner_) {
        return;
    }

    QByteArray schemeBytes = scheme.toUtf8();
    ffi::cow_url_set_scheme(&inner_, schemeBytes.constData());
}

QString CowUrl::scheme() const {
    if (!inner_) {
        return QString();
    }

    return dataToString(ffi::cow_url_scheme(inner_));
}

QString CowUrl::path([[maybe_unused]] ComponentFormattingOptions options) const {
    if (!inner_) {
        return QString();
    }

    return dataToString(ffi::cow_url_path(inner_));
}

QString CowUrl::query([[maybe_unused]] ComponentFormattingOptions options) const {
    if (!inner_) {
        return QString();
    }

    ffi::CowUrlData data = ffi::cow_url_query(inner_);
    if (data.data == nullptr) {
        return QString();
    }

    return dataToString(data);
}

bool CowUrl::hasQuery() const { return inner_ && ffi::cow_url_has_query(inner_); }

QString CowUrl::toString([[maybe_unused]] ComponentFormattingOptions options) const {
    if (!inner_) {
        return QString();
    }

    return dataToString(ffi::cow_url_to_string(inner_));
}

QByteArray CowUrl::toEncoded() const {
    if (!inner_) {
        return QByteArray();
    }

    return dataToByteArray(ffi::cow_url_to_string(inner_));
}

QString CowUrl::host() const {
    if (!inner_) {
        return QString();
    }

    ffi::CowUrlData data = ffi::cow_url_host(inner_);
    if (data.data == nullptr) {
        return QString();
    }

    return dataToString(data);
}

int CowUrl::port() const { return inner_ ? ffi::cow_url_port(inner_) : -1; }

int CowUrl::port(int defaultPort) const {
    int p = inner_ ? ffi::cow_url_port(inner_) : -1;
    return (p < 0) ? defaultPort : p;
}

QString CowUrl::authority() const {
    QString h = host();
    if (h.isEmpty()) {
        return QString();
    }

    int p = port();
    if (p >= 0) {
        return h + ':' + QString::number(p);
    }

    return h;
}

void CowUrl::setHost(const QString &host) {
    if (!inner_) {
        return;
    }

    QByteArray hostBytes = host.toUtf8();
    ffi::cow_url_set_host(&inner_, hostBytes.constData());
}

void CowUrl::setPort(int port) {
    if (!inner_) {
        return;
    }

    ffi::cow_url_set_port(&inner_, port);
}

void CowUrl::setPath(const QString &path, [[maybe_unused]] ParsingMode mode) {
    if (!inner_) {
        return;
    }

    QByteArray pathBytes = path.toUtf8();
    ffi::cow_url_set_path(&inner_, pathBytes.constData());
}

void CowUrl::setQuery(const QString &query) {
    if (!inner_) {
        return;
    }

    if (!query.isEmpty()) {
        QByteArray queryBytes = query.toUtf8();
        ffi::cow_url_set_query(&inner_, queryBytes.constData());
    } else {
        ffi::cow_url_set_query(&inner_, nullptr);
    }
}

void CowUrl::setQuery(const UrlQuery &query) { setQuery(query.toString()); }

bool CowUrl::operator==(const CowUrl &other) const { return toString() == other.toString(); }

bool CowUrl::operator!=(const CowUrl &other) const { return !(*this == other); }

bool CowUrl::operator<(const CowUrl &other) const { return toString() < other.toString(); }

CowUrl CowUrl::resolved(const QString &relative) const {
    if (!isValid() || relative.isEmpty()) {
        return CowUrl();
    }

    QByteArray relativeBytes = relative.toUtf8();
    CowUrl result;
    result.inner_ = ffi::cow_url_join(inner_, relativeBytes.constData());
    return result;
}
