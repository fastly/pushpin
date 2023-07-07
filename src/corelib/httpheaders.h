/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifndef HTTPHEADERS_H
#define HTTPHEADERS_H

#include <QByteArray>
#include <QPair>
#include <QList>

typedef QPair<QByteArray, QByteArray> HttpHeaderParameter;

class HttpHeaderParameters : public QList<HttpHeaderParameter>
{
public:
	bool contains(const QByteArray &key) const;
	QByteArray get(const QByteArray &key) const;
};

typedef QPair<QByteArray, QByteArray> HttpHeader;

class HttpHeaders : public QList<HttpHeader>
{
public:
	enum ParseMode
	{
		NoParseFirstParameter,
		ParseAllParameters
	};

	bool contains(const QByteArray &key) const;
	QByteArray get(const QByteArray &key) const;
	HttpHeaderParameters getAsParameters(const QByteArray &key, ParseMode mode = NoParseFirstParameter) const;
	QByteArray getAsFirstParameter(const QByteArray &key) const;
	QList<QByteArray> getAll(const QByteArray &key, bool split = true) const;
	QList<HttpHeaderParameters> getAllAsParameters(const QByteArray &key, ParseMode mode = NoParseFirstParameter, bool split = true) const;
	QList<QByteArray> takeAll(const QByteArray &key, bool split = true);
	void removeAll(const QByteArray &key);

	static QByteArray join(const QList<QByteArray> &values);
	static HttpHeaderParameters parseParameters(const QByteArray &in, ParseMode mode = NoParseFirstParameter, bool *ok = 0);
};

#endif
