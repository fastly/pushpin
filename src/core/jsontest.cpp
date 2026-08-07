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
#include "qtcompat.h"
#include "test.h"
#include "variant.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

static void variantMapSerialization() {
    VariantMap vmap;
    vmap["string"] = "test value";
    vmap["number"] = 42;
    vmap["boolean"] = true;
    vmap["null"] = Variant();

    QByteArray json = Json::toString(vmap);
    TEST_ASSERT(!json.isEmpty());

    // Should be valid JSON
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    TEST_ASSERT_EQ(error.error, QJsonParseError::NoError);
    TEST_ASSERT_EQ(doc.isObject(), true);

    QJsonObject obj = doc.object();
    TEST_ASSERT_EQ(obj["string"].toString(), QString("test value"));
    TEST_ASSERT_EQ(obj["number"].toInt(), 42);
    TEST_ASSERT_EQ(obj["boolean"].toBool(), true);
    TEST_ASSERT_EQ(obj["null"].isNull(), true);
}

static void variantListSerialization() {
    VariantList vlist;
    vlist << "item1" << 123 << false << Variant();

    QByteArray json = Json::toString(vlist);
    TEST_ASSERT(!json.isEmpty());

    // Should be valid JSON array
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    TEST_ASSERT_EQ(error.error, QJsonParseError::NoError);
    TEST_ASSERT_EQ(doc.isArray(), true);

    QJsonArray arr = doc.array();
    TEST_ASSERT_EQ(arr.size(), 4);
    TEST_ASSERT_EQ(arr[0].toString(), QString("item1"));
    TEST_ASSERT_EQ(arr[1].toInt(), 123);
    TEST_ASSERT_EQ(arr[2].toBool(), false);
    TEST_ASSERT_EQ(arr[3].isNull(), true);
}

static void variantHashSerialization() {
    VariantHash vhash;
    vhash["key1"] = "value1";
    vhash["key2"] = 999;

    QByteArray json = Json::toString(vhash);
    TEST_ASSERT(!json.isEmpty());

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    TEST_ASSERT_EQ(error.error, QJsonParseError::NoError);
    TEST_ASSERT_EQ(doc.isObject(), true);
}

static void nestedStructure() {
    VariantList innerList;
    innerList << 1 << 2 << 3;

    VariantMap innerMap;
    innerMap["nested_key"] = "nested_value";
    innerMap["nested_list"] = innerList;

    VariantMap outerMap;
    outerMap["inner"] = innerMap;
    outerMap["simple"] = "value";

    QByteArray json = Json::toString(outerMap);
    TEST_ASSERT(!json.isEmpty());

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    TEST_ASSERT_EQ(error.error, QJsonParseError::NoError);

    QJsonObject obj = doc.object();
    TEST_ASSERT_EQ(obj.contains("inner"), true);
    TEST_ASSERT_EQ(obj["inner"].isObject(), true);

    QJsonObject inner = obj["inner"].toObject();
    TEST_ASSERT_EQ(inner["nested_key"].toString(), QString("nested_value"));
    TEST_ASSERT_EQ(inner["nested_list"].isArray(), true);
}

static void roundtrip() {
    VariantMap original;
    original["test"] = "roundtrip";
    original["number"] = 42;

    QByteArray json = Json::toString(original);

    // Deserialize back
    Variant restored = Json::fromString(json);
    TEST_ASSERT_EQ(restored.isValid(), true);
    TEST_ASSERT(typeId(restored) == VariantType::Map);

    VariantMap restoredMap = restored.toMap();
    TEST_ASSERT_EQ(restoredMap["test"].toString(), original["test"].toString());
    TEST_ASSERT_EQ(restoredMap["number"].toInt(), original["number"].toInt());
}

static void compatibilityWithQtJson() {
    VariantMap testData;
    testData["string"] = "test";
    testData["number"] = 42;
    testData["bool"] = true;

    QByteArray qtJson =
        QJsonDocument(QJsonObject::fromVariantMap(testData)).toJson(QJsonDocument::Compact);

    QByteArray json = Json::toString(testData);

    // Both should parse to equivalent structures
    QJsonDocument qtDoc = QJsonDocument::fromJson(qtJson);
    QJsonDocument doc = QJsonDocument::fromJson(json);

    TEST_ASSERT_EQ(qtDoc.isObject(), true);
    TEST_ASSERT_EQ(doc.isObject(), true);

    // The objects should be equivalent (though order might differ)
    QJsonObject qtObj = qtDoc.object();
    QJsonObject obj = doc.object();

    TEST_ASSERT_EQ(obj["string"], qtObj["string"]);
    TEST_ASSERT_EQ(obj["number"], qtObj["number"]);
    TEST_ASSERT_EQ(obj["bool"], qtObj["bool"]);
}

static void scalarValues() {
    // Test individual scalar values
    Variant stringVar("test string");
    QByteArray stringJson = Json::toString(stringVar);
    TEST_ASSERT(!stringJson.isEmpty());
    // Should produce clean scalar: "test string"
    TEST_ASSERT_EQ(stringJson, QByteArray("\"test string\""));

    Variant intVar(123);
    QByteArray intJson = Json::toString(intVar);
    TEST_ASSERT(!intJson.isEmpty());
    // Should produce clean scalar: 123
    TEST_ASSERT_EQ(intJson, QByteArray("123"));

    Variant boolVar(true);
    QByteArray boolJson = Json::toString(boolVar);
    TEST_ASSERT(!boolJson.isEmpty());
    // Should produce clean scalar: true
    TEST_ASSERT_EQ(boolJson, QByteArray("true"));

    // For Qt compatibility testing, wrap scalars in objects since Qt JSON only handles
    // objects/arrays
    QByteArray wrappedStringJson = "{\"value\":" + stringJson + "}";
    QByteArray wrappedIntJson = "{\"value\":" + intJson + "}";
    QByteArray wrappedBoolJson = "{\"value\":" + boolJson + "}";

    // Wrapped versions should be parseable by Qt JSON
    QJsonParseError error1, error2, error3;
    QJsonDocument doc1 = QJsonDocument::fromJson(wrappedStringJson, &error1);
    QJsonDocument doc2 = QJsonDocument::fromJson(wrappedIntJson, &error2);
    QJsonDocument doc3 = QJsonDocument::fromJson(wrappedBoolJson, &error3);

    TEST_ASSERT_EQ(error1.error, QJsonParseError::NoError);
    TEST_ASSERT_EQ(error2.error, QJsonParseError::NoError);
    TEST_ASSERT_EQ(error3.error, QJsonParseError::NoError);
}

extern "C" int json_test(ffi::TestException *out_ex) {
    TEST_CATCH(variantMapSerialization());
    TEST_CATCH(variantListSerialization());
    TEST_CATCH(variantHashSerialization());
    TEST_CATCH(nestedStructure());
    TEST_CATCH(roundtrip());
    TEST_CATCH(compatibilityWithQtJson());
    TEST_CATCH(scalarValues());

    return 0;
}