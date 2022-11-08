/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#ifndef JWT_H
#define JWT_H

#include <QByteArray>
#include <QVariant>

class QString;

namespace Jwt {

// NOTE: must match values on the rust side
enum Algorithm
{
	HS256 = 0,
	ES256 = 1,
	RS256 = 2,
};

class EncodingKey
{
public:
	~EncodingKey();

	bool isNull() const { return (bool)(!raw_); }
	const void *raw() const { return raw_; }

	static EncodingKey fromSecret(const QByteArray &key);
	static EncodingKey fromEcPem(const QByteArray &key);
	static EncodingKey fromEcPemFile(const QString &fileName);

private:
	void *raw_;

	EncodingKey() { raw_ = 0; }
};

class DecodingKey
{
public:
	~DecodingKey();

	bool isNull() const { return (bool)(!raw_); }
	const void *raw() const { return raw_; }

	static DecodingKey fromSecret(const QByteArray &key);
	static DecodingKey fromEcPem(const QByteArray &key);
	static DecodingKey fromEcPemFile(const QString &fileName);

private:
	void *raw_;

	DecodingKey() { raw_ = 0; }
};

// returns token, null on error
QByteArray encodeWithAlgorithm(Algorithm alg, const QByteArray &claim, const EncodingKey &key);

// returns claim, null on error
QByteArray decodeWithAlgorithm(Algorithm alg, const QByteArray &token, const DecodingKey &key);

QByteArray encode(const QVariant &claim, const QByteArray &key);
QVariant decode(const QByteArray &token, const QByteArray &key);

}

#endif
