/*
 * Copyright (C) 2016 Fanout, Inc.
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
#include "template.h"

class TemplateTest : public QObject
{
	Q_OBJECT

private slots:
	void render()
	{
		QVariantMap context;
		context["place"] = "world";

		QVariantMap user;
		user["first"] = "john";
		user["last"] = "smith";
		context["user"] = user;

		QVariantList fruits;
		fruits.append("apple");
		fruits.append("banana");
		context["fruits"] = fruits;

		QString content("hello {{ place }}!");
		QString output = Template::render(content, context);
		QCOMPARE(output, QString("hello world!"));

		content = QString("hello {% if formal %}{{ user.last }}{% endif %}{% if not formal %}{{ user.first }}{% endif %}!");
		output = Template::render(content, context);
		QCOMPARE(output, QString("hello john!"));
		context["formal"] = true;
		output = Template::render(content, context);
		QCOMPARE(output, QString("hello smith!"));

		content = QString("please eat {% for f in fruits %}{% if not loop.first %} and {% endif %}fresh {{ f }}s{% endfor %}.");
		output = Template::render(content, context);
		QCOMPARE(output, QString("please eat fresh apples and fresh bananas."));
	}
};

QTEST_MAIN(TemplateTest)
#include "templatetest.moc"
