/*
 * Copyright (C) 2015 Fanout, Inc.
 *
 * This file is part of Pushpin.
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
 */

#include "updater.h"

#include <QSysInfo>
#include <QTimer>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include "log.h"
#include "httpheaders.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"

#define CHECK_INTERVAL (24 * 60 * 60 * 1000)
#define CHECK_URL "https://updates.fanout.io/check/"
#define USER_AGENT "Pushpin-Updater"
#define MAX_RESPONSE_SIZE 50000

static QString getOs()
{
#if defined(Q_OS_MAC)
	return "mac";
#elif defined(Q_OS_LINUX)
	return "linux";
#elif defined(Q_OS_FREEBSD)
	return "freebsd";
#elif defined(Q_OS_NETBSD)
	return "netbsd";
#elif defined(Q_OS_OPENBSD)
	return "openbsd";
#elif defined(Q_OS_UNIX)
	return "unix";
#else
	return QString();
#endif
}

static QString getArch()
{
	return QString::number(sizeof(void *) * 8);
}

class Updater::Private : public QObject
{
	Q_OBJECT

public:
	Updater *q;
	QString currentVersion;
	ZhttpManager *zhttpManager;
	QTimer *timer;
	ZhttpRequest *req;

	Private(Updater *_q, const QString &_currentVersion, ZhttpManager *zhttp) :
		QObject(_q),
		q(_q),
		currentVersion(_currentVersion),
		zhttpManager(zhttp),
		req(0)
	{
		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
		timer->setInterval(CHECK_INTERVAL);
		timer->start();
	}

	~Private()
	{
		timer->disconnect(this);
		timer->setParent(0);
		timer->deleteLater();
	}

	void cleanupRequest()
	{
		delete req;
		req = 0;
	}

private slots:
	void doCheck()
	{
		req = zhttpManager->createRequest();
		req->setParent(this);
		connect(req, SIGNAL(readyRead()), SLOT(req_readyRead()));
		connect(req, SIGNAL(error()), SLOT(req_error()));

		req->setIgnorePolicies(true);
		req->setIgnoreTlsErrors(true);

		QUrl url(CHECK_URL);

		QUrlQuery query;
		query.addQueryItem("package", "pushpin");
		query.addQueryItem("version", currentVersion);
		QString os = getOs();
		if(!os.isEmpty())
			query.addQueryItem("os", os);
		QString arch = getArch();
		if(!arch.isEmpty())
			query.addQueryItem("arch", arch);

		url.setQuery(query);

		HttpHeaders headers;
		headers += HttpHeader("User-Agent", USER_AGENT);
		log_debug("updater: checking for updates: %s", qPrintable(url.toString()));
		req->start("GET", url, headers);
		req->endBody();
	}

	void req_readyRead()
	{
		if(req->bytesAvailable() > MAX_RESPONSE_SIZE)
		{
			log_debug("updater: check failed, response too large");
			cleanupRequest();
			return;
		}

		if(!req->isFinished())
			return;

		if(req->responseCode() != 200)
		{
			log_debug("updater: check failed, response code: %d", req->responseCode());
			cleanupRequest();
			return;
		}

		QByteArray rawBody = req->readBody();
		cleanupRequest();

		QJsonParseError e;
		QJsonDocument doc = QJsonDocument::fromJson(rawBody, &e);
		if(e.error != QJsonParseError::NoError || !doc.isObject())
		{
			log_debug("updater: check failed, unexpected response body format");
			return;
		}

		log_debug("updater: check finished");

		QVariantMap body = doc.object().toVariantMap();

		if(body.contains("updates") && body["updates"].type() == QVariant::List)
		{
			QVariantList updates = body["updates"].toList();
			if(!updates.isEmpty() && updates[0].type() == QVariant::Map)
			{
				QVariantMap update = updates[0].toMap();
				QString version = update.value("version").toString();
				QString link = update.value("link").toString();

				if(!version.isEmpty())
				{
					QString msg = QString("New version of Pushpin available! version=%1").arg(version);
					if(!link.isEmpty())
						msg += QString(" %1").arg(link);
					log_info("%s", qPrintable(msg));
				}
			}
		}
	}

	void req_error()
	{
		log_debug("updater: check failed, req error: %d", (int)req->errorCondition());
		cleanupRequest();
	}

	void timer_timeout()
	{
		if(!req)
			doCheck();
	}
};

Updater::Updater(const QString &currentVersion, ZhttpManager *zhttp, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, currentVersion, zhttp);
}

Updater::~Updater()
{
	delete d;
}

#include "updater.moc"
