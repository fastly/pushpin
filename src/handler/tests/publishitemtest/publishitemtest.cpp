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
#include "publishformat.h"
#include "publishitem.h"

class PublishItemTest : public QObject
{
	Q_OBJECT

private slots:
	void parseItem()
	{
		QVariantHash meta;
		meta["foo"] = QByteArray("bar");
		meta["bar"] = QByteArray("baz");

		QVariantHash hs;
		hs["content"] = QByteArray("hello world");

		QVariantHash formats;
		formats["http-stream"] = hs;

		QVariantHash data;
		data["channel"] = QByteArray("apple");
		data["id"] = QByteArray("item1");
		data["prev-id"] = QByteArray("item0");
		data["meta"] = meta;
		data["formats"] = formats;

		bool ok;
		PublishItem i = PublishItem::fromVariant(data, QString(), &ok);
		QVERIFY(ok);
		QCOMPARE(i.channel, QString("apple"));
		QCOMPARE(i.id, QString("item1"));
		QCOMPARE(i.prevId, QString("item0"));
		QCOMPARE(i.meta.count(), 2);
		QCOMPARE(i.meta.value("foo"), QString("bar"));
		QCOMPARE(i.meta.value("bar"), QString("baz"));
		QVERIFY(i.formats.contains(PublishFormat::HttpStream));
		QCOMPARE(i.formats.value(PublishFormat::HttpStream).body, QByteArray("hello world"));
	}

	void parseItemJsonStyle()
	{
		QVariantMap meta;
		meta["foo"] = QString("bar");
		meta["bar"] = QString("baz");

		QVariantMap hs;
		hs["content"] = QString("hello world");

		QVariantMap formats;
		formats["http-stream"] = hs;

		QVariantMap data;
		data["channel"] = QString("apple");
		data["id"] = QString("item1");
		data["prev-id"] = QString("item0");
		data["meta"] = meta;
		data["formats"] = formats;

		bool ok;
		PublishItem i = PublishItem::fromVariant(data, QString(), &ok);
		QVERIFY(ok);
		QCOMPARE(i.channel, QString("apple"));
		QCOMPARE(i.id, QString("item1"));
		QCOMPARE(i.prevId, QString("item0"));
		QCOMPARE(i.meta.count(), 2);
		QCOMPARE(i.meta.value("foo"), QString("bar"));
		QCOMPARE(i.meta.value("bar"), QString("baz"));
		QVERIFY(i.formats.contains(PublishFormat::HttpStream));
		QCOMPARE(i.formats.value(PublishFormat::HttpStream).body, QByteArray("hello world"));
	}
};

QTEST_MAIN(PublishItemTest)
#include "publishitemtest.moc"
