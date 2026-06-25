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

CowUrl CowUrl::fromEncoded(const QByteArray &input, [[maybe_unused]] ParsingMode mode) {
    CowUrl url;
    url.inner_ = QUrl::fromEncoded(input);
    return url;
}

QString CowUrl::fromPercentEncoding(const QByteArray &input) {
    return QUrl::fromPercentEncoding(input);
}

QByteArray CowUrl::toPercentEncoding(const QString &input) {
    return QUrl::toPercentEncoding(input);
}

bool CowUrl::isValidRelativeUrl(const QString &relativeUrl) {
    CowUrl dummyBase("http://example.com/");
    return dummyBase.resolved(relativeUrl).isValid();
}

void CowUrl::setQuery(const QString &query) {
    if (!query.isEmpty())
        inner_.setQuery(query);
    else
        inner_.setQuery(QString());
}

void CowUrl::setQuery(const UrlQuery &query) { setQuery(query.toString()); }

CowUrl CowUrl::resolved(const QString &relative) const {
    if (!isValid() || relative.isEmpty())
        return CowUrl();

    CowUrl result;
    result.inner_ = inner_.resolved(QUrl(relative));
    return result;
}
