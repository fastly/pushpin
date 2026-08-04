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

#ifndef URLQUERY_H
#define URLQUERY_H

#include "cowurl.h"
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

class UrlQuery {
public:
    // Constructors
    UrlQuery() = default;
    explicit UrlQuery(const CowUrl &url) : inner_(url.query()) {}
    explicit UrlQuery(const QString &queryString) : inner_(queryString) {}
    UrlQuery(const UrlQuery &other) : inner_(other.inner_) {}
    UrlQuery(UrlQuery &&other) noexcept : inner_(std::move(other.inner_)) {}

    // Assignment operators
    UrlQuery &operator=(const UrlQuery &other) {
        if (this != &other) {
            inner_ = other.inner_;
        }
        return *this;
    }
    UrlQuery &operator=(UrlQuery &&other) noexcept {
        if (this != &other) {
            inner_ = std::move(other.inner_);
        }
        return *this;
    }

    // Comparison operators
    bool operator==(const UrlQuery &other) const { return inner_ == other.inner_; }
    bool operator!=(const UrlQuery &other) const { return inner_ != other.inner_; }

    // Query manipulation
    void clear() { inner_.clear(); }
    bool isEmpty() const { return inner_.isEmpty(); }

    // Item management
    void addQueryItem(const QString &key, const QString &value) { inner_.addQueryItem(key, value); }
    void removeQueryItem(const QString &key) { inner_.removeQueryItem(key); }
    void removeAllQueryItems(const QString &key) { inner_.removeAllQueryItems(key); }

    // Item access
    bool hasQueryItem(const QString &key) const { return inner_.hasQueryItem(key); }
    QString
    queryItemValue(const QString &key,
                   CowUrl::ComponentFormattingOptions encoding = CowUrl::PrettyDecoded) const {
        return inner_.queryItemValue(key, static_cast<QUrl::ComponentFormattingOptions>(encoding));
    }
    QStringList
    allQueryItemValues(const QString &key,
                       CowUrl::ComponentFormattingOptions encoding = CowUrl::PrettyDecoded) const {
        return inner_.allQueryItemValues(key,
                                         static_cast<QUrl::ComponentFormattingOptions>(encoding));
    }
    QList<QPair<QString, QString>>
    queryItems(CowUrl::ComponentFormattingOptions encoding = CowUrl::PrettyDecoded) const {
        return inner_.queryItems(static_cast<QUrl::ComponentFormattingOptions>(encoding));
    }

    // String conversion
    QString toString(CowUrl::ComponentFormattingOptions encoding = CowUrl::PrettyDecoded) const {
        return inner_.toString(static_cast<QUrl::ComponentFormattingOptions>(encoding));
    }

    // Access to underlying QUrlQuery for compatibility
    const QUrlQuery &asQUrlQuery() const { return inner_; }
    QUrlQuery &asQUrlQuery() { return inner_; }

private:
    QUrlQuery inner_;
};

#endif // URLQUERY_H
