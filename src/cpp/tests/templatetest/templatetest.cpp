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
