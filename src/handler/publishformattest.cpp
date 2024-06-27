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

namespace {
namespace Main {
QTEST_MAIN(PublishFormatTest)
}
}

extern "C" {

int publishformat_test(int argc, char **argv)
{
	return Main::main(argc, argv);
}

}

#include "publishformattest.moc"
