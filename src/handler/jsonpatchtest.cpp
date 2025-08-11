/*
 * Copyright (C) 2015 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#include "test.h"
#include "qtcompat.h"
#include "jsonpatch.h"

static void patch()
{
	QVariantMap data;
	data["foo"] = "bar";

	QVariantMap op;
	op["op"] = "test";
	op["path"] = "/foo";
	op["value"] = "bar";
	QString msg;
	QVariant ret = JsonPatch::patch(data, QVariantList() << op, &msg);
	TEST_ASSERT(ret.isValid());
	data = ret.toMap();

	op.clear();
	op["op"] = "add";
	op["path"] = "/fruit";
	op["value"] = QVariantList() << "apple";
	ret = JsonPatch::patch(data, QVariantList() << op);
	TEST_ASSERT(ret.isValid());
	data = ret.toMap();
	TEST_ASSERT_EQ(typeId(data["fruit"]), QMetaType::QVariantList);
	TEST_ASSERT_EQ(data["fruit"].toList()[0].toString(), QString("apple"));

	op.clear();
	op["op"] = "copy";
	op["from"] = "/foo";
	op["path"] = "/fruit/-";
	ret = JsonPatch::patch(data, QVariantList() << op);
	TEST_ASSERT(ret.isValid());
	data = ret.toMap();
	TEST_ASSERT_EQ(data["fruit"].toList()[1].toString(), QString("bar"));

	op.clear();
	op["op"] = "replace";
	op["path"] = "/fruit/1";
	QVariantMap bowl;
	bowl["cherries"] = true;
	bowl["grapes"] = 5;
	op["value"] = bowl;
	ret = JsonPatch::patch(data, QVariantList() << op);
	TEST_ASSERT(ret.isValid());
	data = ret.toMap();
	TEST_ASSERT_EQ(typeId(data["fruit"].toList()[1]), QMetaType::QVariantMap);
	TEST_ASSERT_EQ(data["fruit"].toList()[1].toMap().value("cherries").toBool(), true);
	TEST_ASSERT_EQ(data["fruit"].toList()[1].toMap().value("grapes").toInt(), 5);

	op.clear();
	op["op"] = "remove";
	op["path"] = "/fruit/1/cherries";
	ret = JsonPatch::patch(data, QVariantList() << op);
	TEST_ASSERT(ret.isValid());
	data = ret.toMap();
	TEST_ASSERT(!data["fruit"].toList()[1].toMap().contains("cherries"));
	TEST_ASSERT_EQ(data["fruit"].toList()[1].toMap().value("grapes").toInt(), 5);

	op.clear();
	op["op"] = "move";
	op["from"] = "/fruit/0";
	op["path"] = "/foo";
	ret = JsonPatch::patch(data, QVariantList() << op);
	TEST_ASSERT(ret.isValid());
	data = ret.toMap();
	TEST_ASSERT_EQ(data["foo"].toString(), QString("apple"));
	TEST_ASSERT_EQ(data["fruit"].toList()[0].toMap().value("grapes").toInt(), 5);
}

extern "C" int jsonpatch_test(ffi::TestException *out_ex)
{
	TEST_CATCH(patch());

	return 0;
}
