/*
 * Copyright (C) 2017 Fanout, Inc.
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
#include "idformat.h"

class IdFormatTest : public QObject
{
	Q_OBJECT

private slots:
	void renderId()
	{
		QHash<QString, QByteArray> vars;
		QByteArray sformat = "This template has no directives.";
		QByteArray ret = IdFormat::renderId(sformat, vars);
		QCOMPARE(ret, QByteArray("This template has no directives."));

		vars["name"] = "Alice";
		vars["food\\fruit(type)"] = "apples";

		sformat = "My name is %(name)s and I eat %(food\\\\fruit(type\\))s 10%% of the time.";
		ret = IdFormat::renderId(sformat, vars);
		QCOMPARE(ret, QByteArray("My name is Alice and I eat apples 10% of the time."));
	}

	void renderContent()
	{
		QByteArray id = "C3PO";
		QByteArray content = "This content has no directives.";
		QByteArray ret = IdFormat::ContentRenderer(id, false).process(content);
		QCOMPARE(ret, QByteArray("This content has no directives."));

		content = "The ID is %I.";
		ret = IdFormat::ContentRenderer(id, false).process(content);
		QCOMPARE(ret, QByteArray("The ID is C3PO."));

		ret = IdFormat::ContentRenderer(id, true).process(content);
		QCOMPARE(ret, QByteArray("The ID is 4333504f."));

		content = "The ID is %(R2D2)I.";
		ret = IdFormat::ContentRenderer(id, true).process(content);
		QCOMPARE(ret, QByteArray("The ID is 52324432."));
	}

	void renderContentIncremental()
	{
		IdFormat::ContentRenderer cr(QByteArray(), true);

		QByteArray ret = cr.update("The ID is %");
		QCOMPARE(ret, QByteArray("The ID is "));
		ret += cr.update("(");
		QCOMPARE(ret, QByteArray("The ID is "));
		ret += cr.update("R2D");
		QCOMPARE(ret, QByteArray("The ID is "));
		ret += cr.update("2");
		QCOMPARE(ret, QByteArray("The ID is "));
		ret += cr.update(")");
		QCOMPARE(ret, QByteArray("The ID is "));
		ret += cr.update("I.");
		QCOMPARE(ret, QByteArray("The ID is 52324432."));

		ret += cr.finalize();
		QVERIFY(!ret.isNull());
		QCOMPARE(ret, QByteArray("The ID is 52324432."));
	}
};

extern "C" {

int idformat_test(int argc, char **argv)
{
	QTEST_MAIN_IMPL(IdFormatTest)
}

}

#include "idformattest.moc"
