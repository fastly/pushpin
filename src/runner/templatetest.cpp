/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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
#include "template.h"

static void render()
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
	TEST_ASSERT_EQ(output, QString("hello world!"));

	content = QString("hello {% if formal %}{{ user.last }}{% endif %}{% if not formal %}{{ user.first }}{% endif %}!");
	output = Template::render(content, context);
	TEST_ASSERT_EQ(output, QString("hello john!"));
	context["formal"] = true;
	output = Template::render(content, context);
	TEST_ASSERT_EQ(output, QString("hello smith!"));

	content = QString("please eat {% for f in fruits %}{% if not loop.first %} and {% endif %}fresh {{ f }}s{% endfor %}.");
	output = Template::render(content, context);
	TEST_ASSERT_EQ(output, QString("please eat fresh apples and fresh bananas."));
}

extern "C" int template_test(ffi::TestException *out_ex)
{
	TEST_CATCH(render());

	return 0;
}
