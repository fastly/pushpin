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

#include "cowstring.h"
#include "urlquery.h"
#include <assert.h>

static CowString dataToString(ffi::CowUrlData data) {
    assert(data.data);

    CowByteArray ba(data.data, data.len);
    ffi::cow_url_data_destroy(data);
    return CowString::fromUtf8(ba);
}

static CowByteArray dataToByteArray(ffi::CowUrlData data) {
    assert(data.data);

    CowByteArray ba(data.data, data.len);
    ffi::cow_url_data_destroy(data);
    return ba;
}

CowUrl::CowUrl() : inner_(nullptr) {}

CowUrl::CowUrl(const CowString &url) {
    CowByteArray utf8 = url.toUtf8();
    inner_ = ffi::cow_url_from_string(utf8.constData());
}

CowUrl::CowUrl(const CowString &url, [[maybe_unused]] ParsingMode mode) {
    CowByteArray utf8 = url.toUtf8();
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

CowUrl CowUrl::fromEncoded(const CowByteArray &input, [[maybe_unused]] ParsingMode mode) {
    CowUrl url;
    url.inner_ = ffi::cow_url_from_string(input.constData());
    return url;
}

CowString CowUrl::fromPercentEncoding(const CowByteArray &input) {
    return CowString::fromUtf8(CowByteArray::fromPercentEncoding(input));
}

CowByteArray CowUrl::toPercentEncoding(const CowString &input) {
    return input.toUtf8().asQByteArray().toPercentEncoding();
}

bool CowUrl::isValidRelativeUrl(const CowString &relativeUrl) {
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

void CowUrl::setScheme(const CowString &scheme) {
    if (!inner_) {
        return;
    }

    CowByteArray schemeBytes = scheme.toUtf8();
    ffi::cow_url_set_scheme(&inner_, schemeBytes.constData());
}

CowString CowUrl::scheme() const {
    if (!inner_) {
        return CowString();
    }

    return dataToString(ffi::cow_url_scheme(inner_));
}

CowString CowUrl::path([[maybe_unused]] ComponentFormattingOptions options) const {
    if (!inner_) {
        return CowString();
    }

    return dataToString(ffi::cow_url_path(inner_));
}

CowString CowUrl::query([[maybe_unused]] ComponentFormattingOptions options) const {
    if (!inner_) {
        return CowString();
    }

    ffi::CowUrlData data = ffi::cow_url_query(inner_);
    if (data.data == nullptr) {
        return CowString();
    }

    return dataToString(data);
}

bool CowUrl::hasQuery() const { return inner_ && ffi::cow_url_has_query(inner_); }

CowString CowUrl::toString([[maybe_unused]] ComponentFormattingOptions options) const {
    if (!inner_) {
        return CowString();
    }

    return dataToString(ffi::cow_url_to_string(inner_));
}

CowByteArray CowUrl::toEncoded() const {
    if (!inner_) {
        return CowByteArray();
    }

    return dataToByteArray(ffi::cow_url_to_string(inner_));
}

CowString CowUrl::host() const {
    if (!inner_) {
        return CowString();
    }

    ffi::CowUrlData data = ffi::cow_url_host(inner_);
    if (data.data == nullptr) {
        return CowString();
    }

    return dataToString(data);
}

int CowUrl::port() const { return inner_ ? ffi::cow_url_port(inner_) : -1; }

int CowUrl::port(int defaultPort) const {
    int p = inner_ ? ffi::cow_url_port(inner_) : -1;
    return (p < 0) ? defaultPort : p;
}

CowString CowUrl::authority() const {
    CowString h = host();
    if (h.isEmpty()) {
        return CowString();
    }

    int p = port();
    if (p >= 0) {
        return h + ":" + CowString::number(p);
    }

    return h;
}

void CowUrl::setHost(const CowString &host) {
    if (!inner_) {
        return;
    }

    CowByteArray hostBytes = host.toUtf8();
    ffi::cow_url_set_host(&inner_, hostBytes.constData());
}

void CowUrl::setPort(int port) {
    if (!inner_) {
        return;
    }

    ffi::cow_url_set_port(&inner_, port);
}

void CowUrl::setPath(const CowString &path, [[maybe_unused]] ParsingMode mode) {
    if (!inner_) {
        return;
    }

    CowByteArray pathBytes = path.toUtf8();
    ffi::cow_url_set_path(&inner_, pathBytes.constData());
}

void CowUrl::setQuery(const CowString &query) {
    if (!inner_) {
        return;
    }

    if (!query.isEmpty()) {
        CowByteArray queryBytes = query.toUtf8();
        ffi::cow_url_set_query(&inner_, queryBytes.constData());
    } else {
        ffi::cow_url_set_query(&inner_, nullptr);
    }
}

void CowUrl::setQuery(const UrlQuery &query) { setQuery(query.toString()); }

bool CowUrl::operator==(const CowUrl &other) const {
    if (!inner_ && !other.inner_)
        return true;
    if (!inner_ || !other.inner_)
        return false;
    return ffi::cow_url_eq(inner_, other.inner_) == 1;
}

bool CowUrl::operator!=(const CowUrl &other) const { return !(*this == other); }

bool CowUrl::operator<(const CowUrl &other) const {
    if (!inner_)
        return other.inner_ != nullptr;
    if (!other.inner_)
        return false;
    return ffi::cow_url_cmp(inner_, other.inner_) < 0;
}

CowUrl CowUrl::resolved(const CowString &relative) const {
    if (!isValid() || relative.isEmpty()) {
        return CowUrl();
    }

    CowByteArray relativeBytes = relative.toUtf8();
    CowUrl result;
    result.inner_ = ffi::cow_url_join(inner_, relativeBytes.constData());
    return result;
}
