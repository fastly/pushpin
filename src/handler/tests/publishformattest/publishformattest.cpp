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
#include <QVariant>
#include "publishformat.h"

class PublishFormatTest : public QObject
{
	Q_OBJECT

private slots:
	void responseFormat()
	{
		QVariantHash data;
		data["code"] = 200;
		data["reason"] = QByteArray("OK");
		data["headers"] = QVariantList() << QVariant(QVariantList() << QByteArray("Content-Type") << QByteArray("text/plain"));
		data["body"] = QByteArray("hello world");

		bool ok;
		PublishFormat f = PublishFormat::fromVariant(PublishFormat::HttpResponse, data, &ok);
		QVERIFY(ok);
		QCOMPARE(f.code, 200);
		QCOMPARE(f.reason, QByteArray("OK"));
		QCOMPARE(f.headers.count(), 1);
		QCOMPARE(f.headers[0].first, QByteArray("Content-Type"));
		QCOMPARE(f.headers[0].second, QByteArray("text/plain"));
		QCOMPARE(f.body, QByteArray("hello world"));

		data.clear();
		data["body"] = QByteArray("other fields implied");

		f = PublishFormat::fromVariant(PublishFormat::HttpResponse, data, &ok);
		QVERIFY(ok);
		QCOMPARE(f.code, 200);
		QCOMPARE(f.reason, QByteArray("OK"));
		QCOMPARE(f.headers.count(), 0);
		QCOMPARE(f.body, QByteArray("other fields implied"));
	}

	void streamFormat()
	{
		QVariantHash data;
		data["content"] = QByteArray("hello world");

		bool ok;
		PublishFormat f = PublishFormat::fromVariant(PublishFormat::HttpStream, data, &ok);
		QVERIFY(ok);
		QVERIFY(f.action == PublishFormat::Send);
		QCOMPARE(f.body, QByteArray("hello world"));

		data.clear();
		data["action"] = QByteArray("close");

		f = PublishFormat::fromVariant(PublishFormat::HttpStream, data, &ok);
		QVERIFY(ok);
		QVERIFY(f.action == PublishFormat::Close);
		QVERIFY(f.body.isEmpty());
	}

	void webSocketMessageFormat()
	{
		QVariantHash data;
		data["content"] = QByteArray("hello world");

		bool ok;
		PublishFormat f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
		QVERIFY(ok);
		QVERIFY(f.action == PublishFormat::Send);
		QCOMPARE(f.messageType, PublishFormat::Text);
		QCOMPARE(f.body, QByteArray("hello world"));

		data.clear();
		data["type"] = "binary";
		data["content"] = QByteArray("hello world");

		f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
		QVERIFY(ok);
		QVERIFY(f.action == PublishFormat::Send);
		QCOMPARE(f.messageType, PublishFormat::Binary);
		QCOMPARE(f.body, QByteArray("hello world"));

		data.clear();
		data["content-bin"] = QByteArray("hello world");

		f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
		QVERIFY(ok);
		QVERIFY(f.action == PublishFormat::Send);
		QCOMPARE(f.messageType, PublishFormat::Binary);
		QCOMPARE(f.body, QByteArray("hello world"));

		data.clear();
		data["action"] = "close";

		f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
		QVERIFY(ok);
		QVERIFY(f.action == PublishFormat::Close);
		QCOMPARE(f.code, -1);

		data.clear();
		data["action"] = "close";
		data["code"] = 1001;

		f = PublishFormat::fromVariant(PublishFormat::WebSocketMessage, data, &ok);
		QVERIFY(ok);
		QVERIFY(f.action == PublishFormat::Close);
		QCOMPARE(f.code, 1001);
	}
};

QTEST_MAIN(PublishFormatTest)
#include "publishformattest.moc"
