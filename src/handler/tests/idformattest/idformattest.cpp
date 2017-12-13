/*
 * Copyright (C) 2017 Fanout, Inc.
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

QTEST_MAIN(IdFormatTest)
#include "idformattest.moc"
