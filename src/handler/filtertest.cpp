/*
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

#include <QtTest/QtTest>
#include <boost/signals2.hpp>
#include "filter.h"

class FilterTest : public QObject
{
	Q_OBJECT

private:
	std::tuple<bool, Filter::MessageFilter::Result> runMessageFilters(const QStringList &filterNames, const Filter::Context &context, const QByteArray &content)
	{
		Filter::MessageFilterStack fs(filterNames);

		bool finished = false;
		Filter::MessageFilter::Result r;

		fs.finished.connect([&](const Filter::MessageFilter::Result &_r) {
			finished = true;
			r = _r;
		});

		fs.start(context, content);

		return {finished, r};
	}

private slots:
	void messageFilters()
	{
		QStringList filterNames = QStringList() << "skip-self" << "var-subst";

		Filter::Context context;
		context.subscriptionMeta["user"] = "alice";

		QByteArray content = "hello %(user)s";

		{
			auto [finished, r] = runMessageFilters(filterNames, context, content);
			QVERIFY(finished);
			QVERIFY(r.errorMessage.isNull());
			QCOMPARE(r.sendAction, Filter::Send);
			QCOMPARE(r.content, "hello alice");
		}

		{
			context.publishMeta["sender"] = "alice";
			auto [finished, r] = runMessageFilters(filterNames, context, content);
			QVERIFY(finished);
			QVERIFY(r.errorMessage.isNull());
			QCOMPARE(r.sendAction, Filter::Drop);
		}
	}
};

namespace {
namespace Main {
QTEST_MAIN(FilterTest)
}
}

extern "C" {

int filter_test(int argc, char **argv)
{
	return Main::main(argc, argv);
}

}

#include "filtertest.moc"
