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

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QUrl>

class UrlQuery;

// QUrl-like class that currently forwards to an inner QUrl, to assist with reducing direct
// dependency on Qt. The API is designed to allow cheap conversion to/from QUrl.
class CowUrl {
public:
    enum ParsingMode { StrictMode = 0 };

    enum ComponentFormattingOptions : unsigned int {
        PrettyDecoded = QUrl::PrettyDecoded,
        FullyEncoded = QUrl::FullyEncoded,
        FullyDecoded = QUrl::FullyDecoded
    };

    CowUrl() = default;
    CowUrl(const QString &url, [[maybe_unused]] ParsingMode mode = StrictMode) : inner_(url) {}
    CowUrl(const char *url) : inner_(url) {}
    CowUrl(const CowUrl &other) = default;
    CowUrl &operator=(const CowUrl &other) = default;

    bool operator==(const CowUrl &other) const { return inner_ == other.inner_; }
    bool operator!=(const CowUrl &other) const { return inner_ != other.inner_; }
    bool operator<(const CowUrl &other) const {
        return inner_.toString() < other.inner_.toString();
    }

    static CowUrl fromEncoded(const QByteArray &input, ParsingMode mode = StrictMode);
    static QString fromPercentEncoding(const QByteArray &input);
    static QByteArray toPercentEncoding(const QString &input);
    static bool isValidRelativeUrl(const QString &relativeUrl);

    bool isValid() const { return inner_.isValid() && !inner_.scheme().isEmpty(); }
    bool isEmpty() const { return !isValid(); }
    void clear() { inner_.clear(); }

    void setScheme(const QString &scheme) { inner_.setScheme(scheme); }
    QString scheme() const { return inner_.scheme(); }

    QString path(ComponentFormattingOptions options = FullyEncoded) const {
        return inner_.path(static_cast<QUrl::ComponentFormattingOptions>(options));
    }
    void setPath(const QString &path, [[maybe_unused]] ParsingMode mode = StrictMode) {
        inner_.setPath(path);
    }

    QString query(ComponentFormattingOptions options = FullyEncoded) const {
        if (!inner_.hasQuery())
            return QString();
        return inner_.query(static_cast<QUrl::ComponentFormattingOptions>(options));
    }
    bool hasQuery() const { return inner_.hasQuery(); }
    void setQuery(const QString &query);
    void setQuery(const UrlQuery &query);

    QString host() const { return inner_.host(); }
    void setHost(const QString &host) { inner_.setHost(host); }

    int port() const { return inner_.port(); }
    int port(int defaultPort) const { return inner_.port(defaultPort); }
    void setPort(int port) { inner_.setPort(port); }

    QString authority() const { return inner_.authority(); }

    CowUrl resolved(const QString &relative) const;

    QString toString(ComponentFormattingOptions options = FullyEncoded) const {
        return inner_.toString(static_cast<QUrl::ComponentFormattingOptions>(options));
    }
    QByteArray toEncoded() const { return inner_.toEncoded(); }

private:
    friend uint qHash(const CowUrl &url, uint seed);

    QUrl inner_;
};

// Hash function for CowUrl so it can be used in QHash
inline uint qHash(const CowUrl &url, uint seed = 0) { return qHash(url.inner_, seed); }

#endif // COWURL_H
