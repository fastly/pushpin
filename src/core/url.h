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

#ifndef URL_H
#define URL_H

#include <QString>
#include <QByteArray>
#include <QHash>
#include "rust/bindings.h"

class Url
{
public:
    enum ParsingMode
    {
        StrictMode = 0
    };

    enum ComponentFormattingOption
    {
        FullyEncoded = 0
    };

    Url();
    Url(const QString& url);  // Removed explicit for convenience
    Url(const char* url);     // Added const char* constructor
    Url(const Url& other);
    ~Url();

    Url& operator=(const Url& other);

    bool operator==(const Url& other) const;
    bool operator!=(const Url& other) const;
    bool operator<(const Url& other) const;

    static Url fromEncoded(const QByteArray& input, ParsingMode mode = StrictMode);

    // Validates that a potentially relative URL string is syntactically valid
    // by attempting to resolve it against a dummy base URL
    static bool isValidRelativeUrl(const QString& relativeUrl);

    bool isEmpty() const;
    bool isValid() const;
    void clear();

    void setScheme(const QString& scheme);
    QString scheme() const;
    QString path(ComponentFormattingOption options = FullyEncoded) const;
    QString query(ComponentFormattingOption options = FullyEncoded) const;
    bool hasQuery() const;

    QString host() const;
    int port() const;
    QString authority() const;
    void setHost(const QString& host);
    void setPort(int port);
    void setPath(const QString& path);
    void setQuery(const QString& query);

    Url resolved(const Url& relative) const;
    Url resolved(const QString& relativeString) const;

    QString toString(ComponentFormattingOption options = FullyEncoded) const;
    QByteArray toEncoded() const;

private:
    ffi::CUrlHandle handle_;
};

// Hash function for Url so it can be used in QHash
inline uint qHash(const Url& url, uint seed = 0)
{
    QString urlStr = url.toString();
    return qHashMulti(seed, urlStr);
}

#endif // URL_H