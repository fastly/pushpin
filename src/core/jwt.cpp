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

#include "json.h"
#include "qtcompat.h"
#include "variant.h"
#include <QFile>
#include <QFileInfo>

namespace Jwt {

EncodingKey::Private::Private() : type((KeyType)-1), raw(0) {}

EncodingKey::Private::Private(ffi::JwtEncodingKey key) : type((KeyType)key.type), raw(key.key) {}

EncodingKey::Private::~Private() { ffi::jwt_encoding_key_destroy(raw); }

EncodingKey EncodingKey::fromSecret(const CowByteArray &key) {
    EncodingKey k;
    k.d = new Private(ffi::jwt_encoding_key_from_secret((const uint8_t *)key.data(), key.size()));
    return k;
}

EncodingKey EncodingKey::fromPem(const CowByteArray &key) {
    EncodingKey k;
    k.d = new Private(ffi::jwt_encoding_key_from_pem((const uint8_t *)key.data(), key.size()));
    return k;
}

EncodingKey EncodingKey::fromFile(const QString &fileName) {
    QFile f(fileName);
    if (!f.open(QFile::ReadOnly)) {
        return EncodingKey();
    }

    CowByteArray data = f.readAll();

    if (data.startsWith("-----BEGIN"))
        return fromPem(data);
    else
        return fromSecret(CowByteArray::fromHex(data.trimmed()));
}

EncodingKey EncodingKey::fromConfigString(const QString &s, const QDir &baseDir) {
    if (s.startsWith("file:")) {
        QString keyFile = s.mid(5);
        QFileInfo fi(keyFile);
        if (fi.isRelative())
            keyFile = QFileInfo(baseDir, keyFile).filePath();

        return fromFile(keyFile);
    } else if (!s.isEmpty()) {
        CowByteArray secret;

        if (s.startsWith("base64:"))
            secret = CowByteArray::fromBase64(s.mid(7).toUtf8());
        else
            secret = s.toUtf8();

        return fromSecret(secret);
    } else {
        return EncodingKey();
    }
}

DecodingKey::Private::Private() : type((KeyType)-1), raw(0) {}

DecodingKey::Private::Private(ffi::JwtDecodingKey key) : type((KeyType)key.type), raw(key.key) {}

DecodingKey::Private::~Private() { ffi::jwt_decoding_key_destroy(raw); }

DecodingKey DecodingKey::fromSecret(const CowByteArray &key) {
    DecodingKey k;
    k.d = new Private(ffi::jwt_decoding_key_from_secret((const uint8_t *)key.data(), key.size()));
    return k;
}

DecodingKey DecodingKey::fromPem(const CowByteArray &key) {
    DecodingKey k;
    k.d = new Private(ffi::jwt_decoding_key_from_pem((const uint8_t *)key.data(), key.size()));
    return k;
}

DecodingKey DecodingKey::fromFile(const QString &fileName) {
    QFile f(fileName);
    if (!f.open(QFile::ReadOnly)) {
        return DecodingKey();
    }

    CowByteArray data = f.readAll();

    if (data.startsWith("-----BEGIN"))
        return fromPem(data);
    else
        return fromSecret(CowByteArray::fromHex(data.trimmed()));
}

DecodingKey DecodingKey::fromConfigString(const QString &s, const QDir &baseDir) {
    if (s.startsWith("file:")) {
        QString keyFile = s.mid(5);
        QFileInfo fi(keyFile);
        if (fi.isRelative())
            keyFile = QFileInfo(baseDir, keyFile).filePath();

        return fromFile(keyFile);
    } else if (!s.isEmpty()) {
        CowByteArray secret;

        if (s.startsWith("base64:"))
            secret = CowByteArray::fromBase64(s.mid(7).toUtf8());
        else
            secret = s.toUtf8();

        return fromSecret(secret);
    } else {
        return DecodingKey();
    }
}

CowByteArray encodeWithAlgorithm(Algorithm alg, const CowByteArray &claim, const EncodingKey &key) {
    char *token;

    if (ffi::jwt_encode((int)alg, (const char *)claim.data(), key.raw(), &token) != 0) {
        // Error
        return CowByteArray();
    }

    CowByteArray out = token;
    ffi::jwt_str_destroy(token);

    return out;
}

CowByteArray decodeWithAlgorithm(Algorithm alg, const CowByteArray &token, const DecodingKey &key) {
    char *claim;

    if (ffi::jwt_decode((int)alg, (const char *)token.data(), key.raw(), &claim) != 0) {
        // Error
        return CowByteArray();
    }

    CowByteArray out = claim;
    ffi::jwt_str_destroy(claim);

    return out;
}

CowByteArray encode(const Variant &claim, const EncodingKey &key) {
    Algorithm alg;
    switch (key.type()) {
    case Jwt::KeyType::Secret:
        alg = Jwt::HS256;
        break;
    case Jwt::KeyType::Ec:
        alg = Jwt::ES256;
        break;
    case Jwt::KeyType::Rsa:
        alg = Jwt::RS256;
        break;
    default:
        return CowByteArray();
    }

    CowByteArray claimJson = Json::toString(claim.toMap());
    if (claimJson.isNull())
        return CowByteArray();

    return encodeWithAlgorithm(alg, claimJson, key);
}

Variant decode(const CowByteArray &token, const DecodingKey &key) {
    Algorithm alg;
    switch (key.type()) {
    case Jwt::KeyType::Secret:
        alg = Jwt::HS256;
        break;
    case Jwt::KeyType::Ec:
        alg = Jwt::ES256;
        break;
    case Jwt::KeyType::Rsa:
        alg = Jwt::RS256;
        break;
    default:
        return Variant();
    }

    CowByteArray claimJson = decodeWithAlgorithm(alg, token, key);
    if (claimJson.isEmpty())
        return Variant();

    Variant result = Json::fromString(claimJson);
    if (!result.isValid() || typeId(result) != VariantType::Map)
        return Variant();

    return result;
}

} // namespace Jwt
