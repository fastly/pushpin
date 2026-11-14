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

#ifndef COWURL_H
#define COWURL_H

#include "cowbytearray.h"
#include "cowstring.h"
#include "rust/bindings.h"
#include <QByteArrayView>
#include <QHash>
#include <QUrl>

class UrlQuery;

class CowUrl {
public:
    enum ParsingMode { StrictMode = 0 };

    enum ComponentFormattingOptions : unsigned int {
        PrettyDecoded = QUrl::PrettyDecoded,
        FullyEncoded = QUrl::FullyEncoded,
        FullyDecoded = QUrl::FullyDecoded
    };

    CowUrl();
    CowUrl(const CowString &url);
    CowUrl(const CowString &url, ParsingMode mode);
    CowUrl(const char *url);
    CowUrl(const CowUrl &other);
    ~CowUrl();

    CowUrl &operator=(const CowUrl &other);

    bool operator==(const CowUrl &other) const;
    bool operator!=(const CowUrl &other) const;
    bool operator<(const CowUrl &other) const;

    static CowUrl fromEncoded(const CowByteArray &input, ParsingMode mode = StrictMode);
    static CowString fromPercentEncoding(const CowByteArray &input);
    static CowByteArray toPercentEncoding(const CowString &input);

    // Validates that a potentially relative URL string is syntactically valid
    // by attempting to resolve it against a dummy base URL
    static bool isValidRelativeUrl(const CowString &relativeUrl);

    bool isEmpty() const;
    bool isValid() const;
    void clear();

    void setScheme(const CowString &scheme);
    CowString scheme() const;
    CowString path(ComponentFormattingOptions options = FullyEncoded) const;
    CowString query(ComponentFormattingOptions options = FullyEncoded) const;
    bool hasQuery() const;

    CowString host() const;
    int port() const;
    int port(int defaultPort) const;
    CowString authority() const;
    void setHost(const CowString &host);
    void setPort(int port);
    void setPath(const CowString &path, ParsingMode mode = StrictMode);
    void setQuery(const CowString &query);
    void setQuery(const UrlQuery &query);

    CowUrl resolved(const CowString &relativeString) const;

    CowString toString(ComponentFormattingOptions options = FullyEncoded) const;
    CowByteArray toEncoded() const;

private:
    friend size_t qHash(const CowUrl &url, size_t seed) noexcept;
    ffi::Url *inner_;
};

// Hash function for CowUrl so it can be used in QHash
inline size_t qHash(const CowUrl &url, size_t seed = 0) noexcept {
    if (!url.inner_)
        return seed;
    ffi::CowUrlData s = ffi::cow_url_as_str(url.inner_);
    return qHash(QByteArrayView(s.data, static_cast<qsizetype>(s.len)), seed);
}

#endif // COWURL_H
