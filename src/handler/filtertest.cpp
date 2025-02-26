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

#include <unordered_map>
#include <QtTest/QtTest>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <boost/signals2.hpp>
#include "log.h"
#include "timer.h"
#include "defercall.h"
#include "zhttpmanager.h"
#include "ratelimiter.h"
#include "filter.h"

class HttpFilterServer
{
public:
	std::unique_ptr<ZhttpManager> zhttpIn;
	std::unordered_map<ZhttpRequest*, std::unique_ptr<ZhttpRequest>> reqs;

	HttpFilterServer(const QDir &workDir)
	{
		zhttpIn = std::make_unique<ZhttpManager>();
		zhttpIn->setInstanceId("filter-test-server");
		zhttpIn->setBind(true);
		zhttpIn->setServerInSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("filter-test-in")));
		zhttpIn->setServerInStreamSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("filter-test-in-stream")));
		zhttpIn->setServerOutSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("filter-test-out")));
		zhttpIn->requestReady.connect(boost::bind(&HttpFilterServer::zhttpIn_requestReady, this));
	}

	void zhttpIn_requestReady()
	{
		ZhttpRequest *req = zhttpIn->takeNextRequest();
		if(!req)
			return;

		req->readyRead.connect(boost::bind(&HttpFilterServer::req_readyRead, this, req));
		req->bytesWritten.connect(boost::bind(&HttpFilterServer::req_bytesWritten, this, req, boost::placeholders::_1));

		reqs.emplace(std::make_pair(req, std::unique_ptr<ZhttpRequest>(req)));

		req_readyRead(req);
	}

	void req_readyRead(ZhttpRequest *req)
	{
		if(!req->isInputFinished())
			return;

		handle(req);
	}

	void req_bytesWritten(ZhttpRequest *req, int written)
	{
		Q_UNUSED(written);

		if(!req->isFinished())
			return;

		reqs.erase(req);
	}

	void respond(ZhttpRequest *req, int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		req->beginResponse(code, reason, headers);
		req->writeBody(body);
		req->endBody();
	}

	void respondOk(ZhttpRequest *req, int code, bool accept, const QByteArray &body)
	{
		HttpHeaders headers;
		if(!accept)
			headers += HttpHeader("Action", "drop");

		respond(req, code, "OK", headers, body);
	}

	void respondError(ZhttpRequest *req, int code, const QByteArray &reason, const QByteArray &body)
	{
		respond(req, code, reason, HttpHeaders(), body);
	}

	void handle(ZhttpRequest *req)
	{
		if(req->requestMethod() != "POST")
		{
			respondError(req, 400, "Bad Request", "Method must be POST\n");
			return;
		}

		QUrl uri = req->requestUri();
		QByteArray body = req->readBody();

		if(uri.path() == "/filter/accept")
		{
			respondOk(req, 200, true, "");
		}
		else if(uri.path() == "/filter/drop")
		{
			respondOk(req, 200, false, "");
		}
		else if(uri.path() == "/filter/modify")
		{
			if(req->requestHeaders().get("Grip-Last") != "test; last-id=a")
			{
				respondError(req, 400, "Bad Request", "Unexpected Grip-Last");
				return;
			}

			QJsonDocument subMetaDoc = QJsonDocument::fromJson(req->requestHeaders().get("Sub-Meta"));
			QJsonObject subMeta = subMetaDoc.object();
			QJsonDocument pubMetaDoc = QJsonDocument::fromJson(req->requestHeaders().get("Pub-Meta"));
			QJsonObject pubMeta = pubMetaDoc.object();

			QString prepend = subMeta["prepend"].toString();
			QString append = pubMeta["append"].toString();

			if(!prepend.isEmpty() || !append.isEmpty())
				respondOk(req, 200, true, prepend.toUtf8() + body + append.toUtf8());
			else
				respondOk(req, 204, true, "");
		}
		else if(uri.path() == "/filter/large")
		{
			respondOk(req, 200, true, QByteArray(1001, 'a'));
		}
		else
		{
			respondError(req, 400, "Bad Request", "Bad Request\n");
		}
	}
};

class FilterTest : public QObject
{
	Q_OBJECT

private:
	std::unique_ptr<HttpFilterServer> filterServer;
	std::unique_ptr<ZhttpManager> zhttpOut;
	std::shared_ptr<RateLimiter> limiter;

	Filter::MessageFilter::Result runMessageFilters(const QStringList &filterNames, const Filter::Context &context, const QByteArray &content)
	{
		Filter::MessageFilterStack fs(filterNames);

		bool finished = false;
		Filter::MessageFilter::Result r;

		fs.finished.connect([&](const Filter::MessageFilter::Result &_r) {
			finished = true;
			r = _r;
		});

		fs.start(context, content);

		while(!finished)
			QTest::qWait(10);

		return r;
	}

private slots:
	void initTestCase()
	{
		log_setOutputLevel(LOG_LEVEL_WARNING);

		QDir outDir(qgetenv("OUT_DIR"));
		QDir workDir(QDir::current().relativeFilePath(outDir.filePath("test-work")));

		Timer::init(100);

		filterServer = std::make_unique<HttpFilterServer>(workDir);

		zhttpOut = std::make_unique<ZhttpManager>();
		zhttpOut->setInstanceId("filter-test-client");
		zhttpOut->setClientOutSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("filter-test-in")));
		zhttpOut->setClientOutStreamSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("filter-test-in-stream")));
		zhttpOut->setClientInSpecs(QStringList() << QString("ipc://%1").arg(workDir.filePath("filter-test-out")));

		limiter = std::make_shared<RateLimiter>();

		QTest::qWait(500);
	}

	void cleanupTestCase()
	{
		zhttpOut.reset();
		filterServer.reset();

		// ensure deferred deletes are processed
		QCoreApplication::instance()->sendPostedEvents();

		Timer::deinit();
		DeferCall::cleanup();
	}

	void messageFilters()
	{
		QStringList filterNames = QStringList() << "skip-self" << "var-subst";

		Filter::Context context;
		context.subscriptionMeta["user"] = "alice";

		QByteArray content = "hello %(user)s";

		{
			auto r = runMessageFilters(filterNames, context, content);
			QVERIFY(r.errorMessage.isNull());
			QCOMPARE(r.sendAction, Filter::Send);
			QCOMPARE(r.content, "hello alice");
		}

		{
			context.publishMeta["sender"] = "alice";
			auto r = runMessageFilters(filterNames, context, content);
			QVERIFY(r.errorMessage.isNull());
			QCOMPARE(r.sendAction, Filter::Drop);
		}
	}

	void httpCheck()
	{
		QStringList filterNames = QStringList() << "http-check";

		Filter::Context context;
		context.subscriptionMeta["url"] = "/filter/accept";
		context.zhttpOut = zhttpOut.get();
		context.currentUri = "http://localhost/";
		context.limiter = limiter;

		QByteArray content = "hello world";

		{
			auto r = runMessageFilters(filterNames, context, content);
			QVERIFY(r.errorMessage.isNull());
			QCOMPARE(r.sendAction, Filter::Send);
			QCOMPARE(r.content, "hello world");
		}

		context.subscriptionMeta["url"] = "/filter/drop";

		{
			auto r = runMessageFilters(filterNames, context, content);
			QVERIFY(r.errorMessage.isNull());
			QCOMPARE(r.sendAction, Filter::Drop);
		}

		context.subscriptionMeta["url"] = "/filter/error";

		{
			auto r = runMessageFilters(filterNames, context, content);
			QCOMPARE(r.errorMessage, "unexpected network request status: code=400");
		}
	}

	void httpModify()
	{
		QStringList filterNames = QStringList() << "http-modify";

		Filter::Context context;
		context.prevIds["test"] = "a";
		context.subscriptionMeta["url"] = "/filter/modify";
		context.zhttpOut = zhttpOut.get();
		context.currentUri = "http://localhost/";
		context.limiter = limiter;

		QByteArray content = "hello world";

		{
			auto r = runMessageFilters(filterNames, context, content);
			QVERIFY(r.errorMessage.isNull());
			QCOMPARE(r.sendAction, Filter::Send);
			QCOMPARE(r.content, "hello world");
		}

		context.subscriptionMeta["prepend"] = "<<<";
		context.publishMeta["append"] = ">>>";

		{
			auto r = runMessageFilters(filterNames, context, content);
			QVERIFY(r.errorMessage.isNull());
			QCOMPARE(r.sendAction, Filter::Send);
			QCOMPARE(r.content, "<<<hello world>>>");
		}

		context.subscriptionMeta.clear();
		context.publishMeta.clear();
		context.subscriptionMeta["url"] = "/filter/large";
		context.responseSizeMax = 1000;

		{
			auto r = runMessageFilters(filterNames, context, content);
			QCOMPARE(r.errorMessage, "network response exceeded 1000 bytes");
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
