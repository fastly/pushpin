/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 * Copyright (C) 2023 Fastly, Inc.
 *
 * This file is part of Pushpin.
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

EncodingKey::Private::Private(ffi::JwtEncodingKey key) :
	type((KeyType)key.type),
	raw(key.key)
{
}

EncodingKey::Private::~Private()
{
	ffi::jwt_encoding_key_destroy(raw);
}

EncodingKey EncodingKey::fromSecret(const QByteArray &key)
{
	EncodingKey k;
	k.d = new Private(ffi::jwt_encoding_key_from_secret((const uint8_t *)key.data(), key.size()));
	return k;
}

EncodingKey EncodingKey::fromPem(const QByteArray &key)
{
	EncodingKey k;
	k.d = new Private(ffi::jwt_encoding_key_from_pem((const uint8_t *)key.data(), key.size()));
	return k;
}

EncodingKey EncodingKey::fromFile(const QString &fileName)
{
	QFile f(fileName);
	if(!f.open(QFile::ReadOnly))
	{
		return EncodingKey();
	}

	QByteArray data = f.readAll();

	if(data.startsWith("-----BEGIN"))
		return fromPem(data);
	else
		return fromSecret(QByteArray::fromHex(data.trimmed()));
}

EncodingKey EncodingKey::fromConfigString(const QString &s, const QDir &baseDir)
{
	if(s.startsWith("file:"))
	{
		QString keyFile = s.mid(5);
		QFileInfo fi(keyFile);
		if(fi.isRelative())
			keyFile = QFileInfo(baseDir, keyFile).filePath();

		return fromFile(keyFile);
	}
	else
	{
		QByteArray secret;

		if(s.startsWith("base64:"))
			secret = QByteArray::fromBase64(s.mid(7).toUtf8());
		else
			secret = s.toUtf8();

		return fromSecret(secret);
	}
}

DecodingKey::Private::Private() :
	type((KeyType)-1),
	raw(0)
{
}

DecodingKey::Private::Private(ffi::JwtDecodingKey key) :
	type((KeyType)key.type),
	raw(key.key)
{
}

DecodingKey::Private::~Private()
{
	ffi::jwt_decoding_key_destroy(raw);
}

DecodingKey DecodingKey::fromSecret(const QByteArray &key)
{
	DecodingKey k;
	k.d = new Private(ffi::jwt_decoding_key_from_secret((const uint8_t *)key.data(), key.size()));
	return k;
}

DecodingKey DecodingKey::fromPem(const QByteArray &key)
{
	DecodingKey k;
	k.d = new Private(ffi::jwt_decoding_key_from_pem((const uint8_t *)key.data(), key.size()));
	return k;
}

DecodingKey DecodingKey::fromFile(const QString &fileName)
{
	QFile f(fileName);
	if(!f.open(QFile::ReadOnly))
	{
		return DecodingKey();
	}

	QByteArray data = f.readAll();

	if(data.startsWith("-----BEGIN"))
		return fromPem(data);
	else
		return fromSecret(QByteArray::fromHex(data.trimmed()));
}

DecodingKey DecodingKey::fromConfigString(const QString &s, const QDir &baseDir)
{
	if(s.startsWith("file:"))
	{
		QString keyFile = s.mid(5);
		QFileInfo fi(keyFile);
		if(fi.isRelative())
			keyFile = QFileInfo(baseDir, keyFile).filePath();

		return fromFile(keyFile);
	}
	else
	{
		QByteArray secret;

		if(s.startsWith("base64:"))
			secret = QByteArray::fromBase64(s.mid(7).toUtf8());
		else
			secret = s.toUtf8();

		return fromSecret(secret);
	}
}

QByteArray encodeWithAlgorithm(Algorithm alg, const QByteArray &claim, const EncodingKey &key)
{
	char *token;

	if(ffi::jwt_encode((int)alg, (const char *)claim.data(), key.raw(), &token) != 0)
	{
		// Error
		return QByteArray();
	}

	QByteArray out = QByteArray(token);
	ffi::jwt_str_destroy(token);

	return out;
}

QByteArray decodeWithAlgorithm(Algorithm alg, const QByteArray &token, const DecodingKey &key)
{
	char *claim;

	if(ffi::jwt_decode((int)alg, (const char *)token.data(), key.raw(), &claim) != 0)
	{
		// Error
		return QByteArray();
	}

	QByteArray out = QByteArray(claim);
	ffi::jwt_str_destroy(claim);

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
