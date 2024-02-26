/*
 * Copyright (C) 2015 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include <QtTest/QtTest>
#include "qtcompat.h"
#include "jsonpatch.h"

class JsonPatchTest : public QObject
{
	Q_OBJECT

private slots:
	void patch()
	{
		QVariantMap data;
		data["foo"] = "bar";

		QVariantMap op;
		op["op"] = "test";
		op["path"] = "/foo";
		op["value"] = "bar";
		QString msg;
		QVariant ret = JsonPatch::patch(data, QVariantList() << op, &msg);
		QVERIFY(ret.isValid());
		data = ret.toMap();

		op.clear();
		op["op"] = "add";
		op["path"] = "/fruit";
		op["value"] = QVariantList() << "apple";
		ret = JsonPatch::patch(data, QVariantList() << op);
		QVERIFY(ret.isValid());
		data = ret.toMap();
		QCOMPARE(typeId(data["fruit"]), QMetaType::QVariantList);
		QCOMPARE(data["fruit"].toList()[0].toString(), QString("apple"));

		op.clear();
		op["op"] = "copy";
		op["from"] = "/foo";
		op["path"] = "/fruit/-";
		ret = JsonPatch::patch(data, QVariantList() << op);
		QVERIFY(ret.isValid());
		data = ret.toMap();
		QCOMPARE(data["fruit"].toList()[1].toString(), QString("bar"));

		op.clear();
		op["op"] = "replace";
		op["path"] = "/fruit/1";
		QVariantMap bowl;
		bowl["cherries"] = true;
		bowl["grapes"] = 5;
		op["value"] = bowl;
		ret = JsonPatch::patch(data, QVariantList() << op);
		QVERIFY(ret.isValid());
		data = ret.toMap();
		QCOMPARE(typeId(data["fruit"].toList()[1]), QMetaType::QVariantMap);
		QCOMPARE(data["fruit"].toList()[1].toMap().value("cherries").toBool(), true);
		QCOMPARE(data["fruit"].toList()[1].toMap().value("grapes").toInt(), 5);

		op.clear();
		op["op"] = "remove";
		op["path"] = "/fruit/1/cherries";
		ret = JsonPatch::patch(data, QVariantList() << op);
		QVERIFY(ret.isValid());
		data = ret.toMap();
		QVERIFY(!data["fruit"].toList()[1].toMap().contains("cherries"));
		QCOMPARE(data["fruit"].toList()[1].toMap().value("grapes").toInt(), 5);

		op.clear();
		op["op"] = "move";
		op["from"] = "/fruit/0";
		op["path"] = "/foo";
		ret = JsonPatch::patch(data, QVariantList() << op);
		QVERIFY(ret.isValid());
		data = ret.toMap();
		QCOMPARE(data["foo"].toString(), QString("apple"));
		QCOMPARE(data["fruit"].toList()[0].toMap().value("grapes").toInt(), 5);
	}
};

namespace {
namespace Main {
QTEST_MAIN(JsonPatchTest)
}
}

extern "C" {

int jsonpatch_test(int argc, char **argv)
{
	return Main::main(argc, argv);
}

}

#include "jsonpatchtest.moc"
