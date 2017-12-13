/*
 * Copyright (C) 2015 Fanout, Inc.
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

#include <QtTest/QtTest>
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
		QCOMPARE(data["fruit"].type(), QVariant::List);
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
		QCOMPARE(data["fruit"].toList()[1].type(), QVariant::Map);
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

QTEST_MAIN(JsonPatchTest)
#include "jsonpatchtest.moc"
