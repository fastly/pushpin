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

#include "jwt.h"

#include <QString>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace Jwt {

EncodingKey::Private::Private() :
	type((KeyType)-1),
	raw(0)
{
}

EncodingKey::Private::Private(JwtEncodingKey key) :
	type((KeyType)key.type),
	raw(key.key)
{
}

EncodingKey::Private::~Private()
{
	jwt_encoding_key_destroy(raw);
}

EncodingKey EncodingKey::fromSecret(const QByteArray &key)
{
	EncodingKey k;
	k.d = new Private(jwt_encoding_key_from_secret((const quint8 *)key.data(), key.size()));
	return k;
}

EncodingKey EncodingKey::fromPem(const QByteArray &key)
{
	EncodingKey k;
	k.d = new Private(jwt_encoding_key_from_pem((const quint8 *)key.data(), key.size()));
	return k;
}

EncodingKey EncodingKey::fromPemFile(const QString &fileName)
{
	QFile f(fileName);
	if(!f.open(QFile::ReadOnly))
	{
		return EncodingKey();
	}

	return fromPem(f.readAll());
}

EncodingKey EncodingKey::fromConfigString(const QString &s, const QDir &baseDir)
{
	if(s.startsWith("file:"))
	{
		QString keyFile = s.mid(5);
		QFileInfo fi(keyFile);
		if(fi.isRelative())
			keyFile = QFileInfo(baseDir, keyFile).filePath();

		return EncodingKey::fromPemFile(keyFile);
	}
	else
	{
		QByteArray secret;

		if(s.startsWith("base64:"))
			secret = QByteArray::fromBase64(s.mid(7).toUtf8());
		else
			secret = s.toUtf8();

		return EncodingKey::fromSecret(secret);
	}
}

DecodingKey::Private::Private() :
	type((KeyType)-1),
	raw(0)
{
}

DecodingKey::Private::Private(JwtDecodingKey key) :
	type((KeyType)key.type),
	raw(key.key)
{
}

DecodingKey::Private::~Private()
{
	jwt_decoding_key_destroy(raw);
}

DecodingKey DecodingKey::fromSecret(const QByteArray &key)
{
	DecodingKey k;
	k.d = new Private(jwt_decoding_key_from_secret((const quint8 *)key.data(), key.size()));
	return k;
}

DecodingKey DecodingKey::fromPem(const QByteArray &key)
{
	DecodingKey k;
	k.d = new Private(jwt_decoding_key_from_pem((const quint8 *)key.data(), key.size()));
	return k;
}

DecodingKey DecodingKey::fromPemFile(const QString &fileName)
{
	QFile f(fileName);
	if(!f.open(QFile::ReadOnly))
	{
		return DecodingKey();
	}

	return fromPem(f.readAll());
}

DecodingKey DecodingKey::fromConfigString(const QString &s, const QDir &baseDir)
{
	if(s.startsWith("file:"))
	{
		QString keyFile = s.mid(5);
		QFileInfo fi(keyFile);
		if(fi.isRelative())
			keyFile = QFileInfo(baseDir, keyFile).filePath();

		return DecodingKey::fromPemFile(keyFile);
	}
	else
	{
		QByteArray secret;

		if(s.startsWith("base64:"))
			secret = QByteArray::fromBase64(s.mid(7).toUtf8());
		else
			secret = s.toUtf8();

		return DecodingKey::fromSecret(secret);
	}
}

QByteArray encodeWithAlgorithm(Algorithm alg, const QByteArray &claim, const EncodingKey &key)
{
	char *token;

	if(jwt_encode((int)alg, (const char *)claim.data(), key.raw(), &token) != 0)
	{
		// error
		return QByteArray();
	}

	QByteArray out = QByteArray(token);
	jwt_str_destroy(token);

	return out;
}

QByteArray decodeWithAlgorithm(Algorithm alg, const QByteArray &token, const DecodingKey &key)
{
	char *claim;

	if(jwt_decode((int)alg, (const char *)token.data(), key.raw(), &claim) != 0)
	{
		// error
		return QByteArray();
	}

	QByteArray out = QByteArray(claim);
	jwt_str_destroy(claim);

	return out;
}

QByteArray encode(const QVariant &claim, const EncodingKey &key)
{
	Algorithm alg;
	switch(key.type())
	{
		case Jwt::KeyType::Secret: alg = Jwt::HS256; break;
		case Jwt::KeyType::Ec: alg = Jwt::ES256; break;
		case Jwt::KeyType::Rsa: alg = Jwt::RS256; break;
		default: return QByteArray();
	}

	QByteArray claimJson = QJsonDocument(QJsonObject::fromVariantMap(claim.toMap())).toJson(QJsonDocument::Compact);
	if(claimJson.isNull())
		return QByteArray();

	return encodeWithAlgorithm(alg, claimJson, key);
}

QVariant decode(const QByteArray &token, const DecodingKey &key)
{
	Algorithm alg;
	switch(key.type())
	{
		case Jwt::KeyType::Secret: alg = Jwt::HS256; break;
		case Jwt::KeyType::Ec: alg = Jwt::ES256; break;
		case Jwt::KeyType::Rsa: alg = Jwt::RS256; break;
		default: return QVariant();
	}

	QByteArray claimJson = decodeWithAlgorithm(alg, token, key);
	if(claimJson.isEmpty())
		return QVariant();

	QJsonParseError error;
	QJsonDocument doc = QJsonDocument::fromJson(claimJson, &error);
	if(error.error != QJsonParseError::NoError || !doc.isObject())
		return QVariant();

	return doc.object().toVariantMap();
}

}
