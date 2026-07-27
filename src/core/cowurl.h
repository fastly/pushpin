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

#include "rust/bindings.h"
#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QString>
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
    CowUrl(const QString &url);
    CowUrl(const QString &url, ParsingMode mode);
    CowUrl(const char *url);
    CowUrl(const CowUrl &other);
    ~CowUrl();

    CowUrl &operator=(const CowUrl &other);

    bool operator==(const CowUrl &other) const;
    bool operator!=(const CowUrl &other) const;
    bool operator<(const CowUrl &other) const;

    static CowUrl fromEncoded(const QByteArray &input, ParsingMode mode = StrictMode);
    static QString fromPercentEncoding(const QByteArray &input);
    static QByteArray toPercentEncoding(const QString &input);

    // Validates that a potentially relative URL string is syntactically valid
    // by attempting to resolve it against a dummy base URL
    static bool isValidRelativeUrl(const QString &relativeUrl);

    bool isEmpty() const;
    bool isValid() const;
    void clear();

    void setScheme(const QString &scheme);
    QString scheme() const;
    QString path(ComponentFormattingOptions options = FullyEncoded) const;
    QString query(ComponentFormattingOptions options = FullyEncoded) const;
    bool hasQuery() const;

    QString host() const;
    int port() const;
    int port(int defaultPort) const;
    QString authority() const;
    void setHost(const QString &host);
    void setPort(int port);
    void setPath(const QString &path, ParsingMode mode = StrictMode);
    void setQuery(const QString &query);
    void setQuery(const UrlQuery &query);

    CowUrl resolved(const QString &relativeString) const;

    QString toString(ComponentFormattingOptions options = FullyEncoded) const;
    QByteArray toEncoded() const;

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
