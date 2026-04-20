/*
 * Copyright (C) 2025 Fastly, Inc.
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

#include "url.h"

Url::Url()
{
    handle_ = ffi::url_new();
}

Url::Url(const QString& url)
{
    QByteArray utf8 = url.toUtf8();
    handle_ = ffi::url_from_string(utf8.constData());
}

Url::Url(const char* url)
{
    handle_ = ffi::url_from_string(url);
}

Url::Url(const Url& other)
{
    handle_ = ffi::url_clone(other.handle_);
}

Url::~Url()
{
    if (handle_) {
        ffi::url_delete(handle_);
    }
}

Url& Url::operator=(const Url& other)
{
    if (this != &other) {
        if (handle_) {
            ffi::url_delete(handle_);
        }
        handle_ = ffi::url_clone(other.handle_);
    }
    return *this;
}

Url Url::fromEncoded(const QByteArray& input, ParsingMode mode)
{
    Url url;
    ffi::url_delete(url.handle_);  // Delete the handle created by the constructor
    url.handle_ = ffi::url_from_encoded(reinterpret_cast<const uint8_t*>(input.constData()),
                                        input.size(), static_cast<ffi::UrlParsingMode>(mode));
    return url;
}

bool Url::isValidRelativeUrl(const QString& relativeUrl)
{
    // Validate by attempting to resolve against a dummy base URL
    Url dummyBase("http://example.com/");
    Url resolved = dummyBase.resolved(relativeUrl);
    return resolved.isValid();
}

bool Url::isEmpty() const
{
    return ffi::url_is_empty(handle_);
}

bool Url::isValid() const
{
    return ffi::url_is_valid(handle_);
}

void Url::clear()
{
    ffi::url_clear(handle_);
}

void Url::setScheme(const QString& scheme)
{
    QByteArray utf8 = scheme.toUtf8();
    ffi::url_set_scheme(handle_, utf8.constData());
}

QString Url::scheme() const
{
    char* result = ffi::url_scheme(handle_);
    if (!result) {
        return QString();
    }

    QString scheme = QString::fromUtf8(result);
    ffi::url_string_delete(result);
    return scheme;
}

QString Url::path(ComponentFormattingOption options) const
{
    char* result = ffi::url_path(handle_, static_cast<ffi::UrlEncoding>(options));
    if (!result) {
        return QString();
    }

    QString path = QString::fromUtf8(result);
    ffi::url_string_delete(result);
    return path;
}

QString Url::query(ComponentFormattingOption options) const
{
    char* result = ffi::url_query(handle_, static_cast<ffi::UrlEncoding>(options));
    if (!result) {
        return QString();
    }

    QString query = QString::fromUtf8(result);
    ffi::url_string_delete(result);
    return query;
}

bool Url::hasQuery() const
{
    return ffi::url_has_query(handle_);
}

QString Url::toString(ComponentFormattingOption options) const
{
    char* result = ffi::url_to_string(handle_, static_cast<ffi::UrlEncoding>(options));
    if (!result) {
        return QString();
    }

    QString url = QString::fromUtf8(result);
    ffi::url_string_delete(result);
    return url;
}

QByteArray Url::toEncoded() const
{
    uintptr_t len = 0;
    uint8_t* result = ffi::url_to_encoded(handle_, &len);
    if (!result || len == 0) {
        return QByteArray();
    }

    QByteArray encoded(reinterpret_cast<const char*>(result), len);
    ffi::url_bytes_delete(result);
    return encoded;
}

QString Url::host() const
{
    char* result = ffi::url_host(handle_);
    if (!result) {
        return QString();
    }

    QString host = QString::fromUtf8(result);
    ffi::url_string_delete(result);
    return host;
}

int Url::port() const
{
    return ffi::url_port(handle_);
}

QString Url::authority() const
{
    char* result = ffi::url_authority(handle_);
    if (!result) {
        return QString();
    }

    QString authority = QString::fromUtf8(result);
    ffi::url_string_delete(result);
    return authority;
}

void Url::setHost(const QString& host)
{
    QByteArray hostBytes = host.toUtf8();
    ffi::url_set_host(handle_, hostBytes.constData());
}

void Url::setPort(int port)
{
    ffi::url_set_port(handle_, port);
}

void Url::setPath(const QString& path)
{
    QByteArray pathBytes = path.toUtf8();
    ffi::url_set_path(handle_, pathBytes.constData());
}

void Url::setQuery(const QString& query)
{
    if (query.isEmpty()) {
        ffi::url_set_query(handle_, nullptr);
    } else {
        QByteArray queryBytes = query.toUtf8();
        ffi::url_set_query(handle_, queryBytes.constData());
    }
}

bool Url::operator==(const Url& other) const
{
    // Compare by string representation
    return toString() == other.toString();
}

bool Url::operator!=(const Url& other) const
{
    return !(*this == other);
}

bool Url::operator<(const Url& other) const
{
    // Lexicographic comparison by string representation
    return toString() < other.toString();
}

Url Url::resolved(const Url& relative) const
{
    return resolved(relative.toString());
}

Url Url::resolved(const QString& relativeString) const
{
    if (!handle_ || !isValid()) {
        return Url();
    }

    if (relativeString.isEmpty()) {
        return Url();
    }

    // Use the url crate's proper join method for RFC 3986 compliant URL resolution
    QByteArray relativeUtf8 = relativeString.toUtf8();
    ffi::CUrlHandle resolved_handle = ffi::url_join(handle_, relativeUtf8.constData());

    Url result;
    ffi::url_delete(result.handle_);  // Delete the default handle
    result.handle_ = resolved_handle;  // Use the resolved handle

    return result;
}