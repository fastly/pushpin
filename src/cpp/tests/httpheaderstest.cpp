/*
 * Copyright (C) 2017 Fanout, Inc.
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
 *
 */

#include <QtTest/QtTest>
#include "httpheaders.h"

class HttpHeadersTest : public QObject
{
	Q_OBJECT

private slots:
	void parseParameters()
	{
		HttpHeaders h;
		h += HttpHeader("Fruit", "apple");
		h += HttpHeader("Fruit", "banana");
		h += HttpHeader("Fruit", "cherry");

		QList<HttpHeaderParameters> params = h.getAllAsParameters("Fruit");
		QCOMPARE(params.count(), 3);
		QCOMPARE(params[0][0].first, QByteArray("apple"));
		QCOMPARE(params[1][0].first, QByteArray("banana"));
		QCOMPARE(params[2][0].first, QByteArray("cherry"));

		h.clear();
		h += HttpHeader("Fruit", "apple, banana, cherry");

		params = h.getAllAsParameters("Fruit");
		QCOMPARE(params.count(), 3);
		QCOMPARE(params[0][0].first, QByteArray("apple"));
		QCOMPARE(params[1][0].first, QByteArray("banana"));
		QCOMPARE(params[2][0].first, QByteArray("cherry"));

		h.clear();
		h += HttpHeader("Fruit", "apple; type=\"granny, smith\", banana; type=\"\\\"yellow\\\"\"");

		params = h.getAllAsParameters("Fruit");
		QCOMPARE(params.count(), 2);
		QCOMPARE(params[0][0].first, QByteArray("apple"));
		QCOMPARE(params[0][1].first, QByteArray("type"));
		QCOMPARE(params[0][1].second, QByteArray("granny, smith"));
		QCOMPARE(params[1][0].first, QByteArray("banana"));
		QCOMPARE(params[1][1].first, QByteArray("type"));
		QCOMPARE(params[1][1].second, QByteArray("\"yellow\""));

		h.clear();
		h += HttpHeader("Fruit", "\"apple");

		QList<QByteArray> l = h.getAll("Fruit");
		QCOMPARE(l.count(), 1);
		QCOMPARE(l[0], QByteArray("\"apple"));

		h.clear();
		h += HttpHeader("Fruit", "\"apple\\");

		l = h.getAll("Fruit");
		QCOMPARE(l.count(), 1);
		QCOMPARE(l[0], QByteArray("\"apple\\"));

		h.clear();
		h += HttpHeader("Fruit", "apple; type=gala, banana; type=\"yellow, cherry");

		params = h.getAllAsParameters("Fruit");
		QCOMPARE(params.count(), 1);
		QCOMPARE(params[0][0].first, QByteArray("apple"));
		QCOMPARE(params[0][1].first, QByteArray("type"));
		QCOMPARE(params[0][1].second, QByteArray("gala"));
	}
};

namespace {
namespace Main {
QTEST_MAIN(HttpHeadersTest)
}
}

extern "C" {

int httpheaders_test(int argc, char **argv)
{
	return Main::main(argc, argv);
}

}

#include "httpheaderstest.moc"
