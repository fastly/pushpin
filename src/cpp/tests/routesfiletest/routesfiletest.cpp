/*
 * Copyright (C) 2016 Fanout, Inc.
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
#include "log.h"
#include "routesfile.h"

class RoutesFileTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		//log_setOutputLevel(LOG_LEVEL_WARNING);
	}

	void cleanupTestCase()
	{
	}

	void lineTests()
	{
		QList<RoutesFile::RouteSection> r;
		bool ok;

		r = RoutesFile::parseLine("apple", &ok);
		QVERIFY(ok);
		QCOMPARE(r.count(), 1);
		QCOMPARE(r[0].value, QString("apple"));
		QVERIFY(r[0].props.isEmpty());

		r = RoutesFile::parseLine("apple banana", &ok);
		QVERIFY(ok);
		QCOMPARE(r.count(), 2);
		QCOMPARE(r[0].value, QString("apple"));
		QVERIFY(r[0].props.isEmpty());
		QCOMPARE(r[1].value, QString("banana"));
		QVERIFY(r[1].props.isEmpty());

		r = RoutesFile::parseLine("  apple   banana  # comment", &ok);
		QVERIFY(ok);
		QCOMPARE(r.count(), 2);
		QCOMPARE(r[0].value, QString("apple"));
		QVERIFY(r[0].props.isEmpty());
		QCOMPARE(r[1].value, QString("banana"));
		QVERIFY(r[1].props.isEmpty());

		r = RoutesFile::parseLine("apple,organic,type=gala,from=\"washington, \\\"usa\\\"\"", &ok);
		QVERIFY(ok);
		QCOMPARE(r.count(), 1);
		QCOMPARE(r[0].value, QString("apple"));
		QCOMPARE(r[0].props.count(), 3);
		QVERIFY(r[0].props.contains("organic"));
		QVERIFY(r[0].props.value("organic").isEmpty());
		QCOMPARE(r[0].props.value("type"), QString("gala"));
		QCOMPARE(r[0].props.value("from"), QString("washington, \"usa\""));

		r = RoutesFile::parseLine("apple,organic banana cherry,type=bing", &ok);
		QVERIFY(ok);
		QCOMPARE(r.count(), 3);
		QCOMPARE(r[0].value, QString("apple"));
		QCOMPARE(r[0].props.count(), 1);
		QVERIFY(r[0].props.contains("organic"));
		QVERIFY(r[0].props.value("organic").isEmpty());
		QCOMPARE(r[1].value, QString("banana"));
		QVERIFY(r[1].props.isEmpty());
		QCOMPARE(r[2].value, QString("cherry"));
		QCOMPARE(r[2].props.value("type"), QString("bing"));

		r = RoutesFile::parseLine(",organic", &ok);
		QVERIFY(ok);
		QCOMPARE(r.count(), 1);
		QCOMPARE(r[0].value, QString(""));
		QCOMPARE(r[0].props.count(), 1);
		QVERIFY(r[0].props.contains("organic"));
		QVERIFY(r[0].props.value("organic").isEmpty());

		r = RoutesFile::parseLine("type=gala", &ok);
		QVERIFY(ok);
		QCOMPARE(r.count(), 1);
		QCOMPARE(r[0].value, QString(""));
		QCOMPARE(r[0].props.count(), 1);
		QVERIFY(r[0].props.contains("type"));
		QCOMPARE(r[0].props.value("type"), QString("gala"));

		// unterminated quote
		r = RoutesFile::parseLine("apple,organic,type=\"gala", &ok);
		QVERIFY(!ok);

		// empty prop name
		r = RoutesFile::parseLine("apple,organic,", &ok);
		QVERIFY(!ok);

		// empty prop name
		r = RoutesFile::parseLine("apple,organic,=gala", &ok);
		QVERIFY(!ok);
	}
};

extern "C" {

int routesfile_test(int argc, char **argv)
{
	QTEST_MAIN_IMPL(RoutesFileTest)
}

}

#include "routesfiletest.moc"
