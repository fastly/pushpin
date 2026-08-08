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

#include "json.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

static QJsonValue variantToJsonValue(const Variant &v) {
    switch (typeId(v)) {
    case VariantType::Invalid:
    case VariantType::Nullptr:
        return QJsonValue::Null;
    case VariantType::Bool:
        return QJsonValue(v.toBool());
    case VariantType::Int:
    case VariantType::UInt:
    case VariantType::LongLong:
    case VariantType::ULongLong:
    case VariantType::Float:
    case VariantType::Double:
        return QJsonValue(v.toDouble());
    case VariantType::String:
        return QJsonValue(v.toString());
    case VariantType::ByteArray:
        return QJsonValue(QString::fromUtf8(v.toByteArray()));
    case VariantType::List:
        return QJsonArray::fromVariantList(v.toList());
    case VariantType::Map:
        return QJsonObject::fromVariantMap(v.toMap());
    case VariantType::Hash:
        return QJsonObject::fromVariantHash(v.toHash());
    default:
        return QJsonValue::Null;
    }
}

static Variant jsonValueToVariant(const QJsonValue &v) {
    switch (v.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return Variant();
    case QJsonValue::Bool:
        return Variant(v.toBool());
    case QJsonValue::Double:
        return Variant(v.toDouble());
    case QJsonValue::String:
        return Variant(v.toString());
    case QJsonValue::Array:
        return v.toArray().toVariantList();
    case QJsonValue::Object:
        return v.toObject().toVariantMap();
    }
    return Variant();
}

namespace Json {

QByteArray toString(const Variant &variant) {
    QJsonValue jv = variantToJsonValue(variant);

    if (jv.isObject())
        return QJsonDocument(jv.toObject()).toJson(QJsonDocument::Compact);
    if (jv.isArray())
        return QJsonDocument(jv.toArray()).toJson(QJsonDocument::Compact);

    // For scalars (null, bool, number, string), use the array trick to get proper encoding
    QByteArray arr = QJsonDocument(QJsonArray{jv}).toJson(QJsonDocument::Compact);
    return arr.mid(1, arr.size() - 2); // strip surrounding [ and ]
}

Variant fromString(const QByteArray &json) {
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError)
        return Variant();

    if (doc.isObject())
        return jsonValueToVariant(doc.object());
    else if (doc.isArray())
        return jsonValueToVariant(doc.array());

    return Variant();
}

} // namespace Json
