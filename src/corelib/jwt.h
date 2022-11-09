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
#include <QSharedData>
#include "rust/jwt.h"

class QString;

namespace Jwt {

enum KeyType {
	Secret = JWT_KEYTYPE_SECRET,
	Ec = JWT_KEYTYPE_EC,
	Rsa = JWT_KEYTYPE_RSA,
};

enum Algorithm
{
	HS256 = JWT_ALGORITHM_HS256,
	ES256 = JWT_ALGORITHM_ES256,
	RS256 = JWT_ALGORITHM_RS256,
};

class EncodingKey
{
public:
	bool isNull() const { return !d; }
	KeyType type() const { if(d) { return d->type; } else { return (KeyType)-1; } }

	const void *raw() const { if(d) { return d->raw; } else { return 0; } }

	static EncodingKey fromSecret(const QByteArray &key);
	static EncodingKey fromPem(const QByteArray &key);
	static EncodingKey fromPemFile(const QString &fileName);
	static EncodingKey fromConfigString(const QString &s);

private:
	class Private : public QSharedData
	{
	public:
		KeyType type;
		void *raw;

		Private();
		Private(JwtEncodingKey key);
		~Private();
	};

	QSharedDataPointer<Private> d;
};

class DecodingKey
{
public:
	bool isNull() const { return !d; }
	KeyType type() const { if(d) { return d->type; } else { return (KeyType)-1; } }

	const void *raw() const { if(d) { return d->raw; } else { return 0; } }

	static DecodingKey fromSecret(const QByteArray &key);
	static DecodingKey fromPem(const QByteArray &key);
	static DecodingKey fromPemFile(const QString &fileName);
	static DecodingKey fromConfigString(const QString &s);

private:
	class Private : public QSharedData
	{
	public:
		KeyType type;
		void *raw;

		Private();
		Private(JwtDecodingKey key);
		~Private();
	};

	QSharedDataPointer<Private> d;
};

// returns token, null on error
QByteArray encodeWithAlgorithm(Algorithm alg, const QByteArray &claim, const EncodingKey &key);

// returns claim, null on error
QByteArray decodeWithAlgorithm(Algorithm alg, const QByteArray &token, const DecodingKey &key);

QByteArray encode(const QVariant &claim, const QByteArray &key);
QVariant decode(const QByteArray &token, const QByteArray &key);

}

#endif
