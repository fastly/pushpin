/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
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

#include "proxysession.h"

#include <QSet>
#include <QUrl>
#include "log.h"
#include "inspectdata.h"
#include "acceptdata.h"
#include "m2request.h"
#include "m2response.h"
#include "zurlmanager.h"
#include "zurlrequest.h"
#include "domainmap.h"
#include "requestsession.h"

class ProxySession::Private : public QObject
{
	Q_OBJECT

public:
	ProxySession *q;
	ZurlManager *zurlManager;
	DomainMap *domainMap;
	M2Request *mr;
	ZurlRequest *zr;
	bool firstRead;
	bool instruct;
	bool haveInspectData;
	InspectData idata;
	AcceptData ad;
	QSet<QByteArray> instructTypes;
	M2Response *resp;

	Private(ProxySession *_q, ZurlManager *_zurlManager, DomainMap *_domainMap) :
		QObject(_q),
		q(_q),
		zurlManager(_zurlManager),
		domainMap(_domainMap),
		haveInspectData(false)
	{
		instruct = false;

		instructTypes += "application/x-fo-instruct";
		instructTypes += "application/fo-instruct";
		instructTypes += "application/grip-instruct";
	}

	void add(RequestSession *rs)
	{
		mr = rs->request();
		connect(mr, SIGNAL(readyRead()), SLOT(mr_readyRead()));
		connect(mr, SIGNAL(finished()), SLOT(mr_finished()));

		zr = zurlManager->createRequest();
		connect(zr, SIGNAL(readyRead()), SLOT(zr_readyRead()));
		connect(zr, SIGNAL(bytesWritten(int)), SLOT(zr_bytesWritten(int)));
		connect(zr, SIGNAL(error()), SLOT(zr_error()));
		QString host = QString::fromUtf8(mr->headers().get("host"));
		int at = host.indexOf(':');
		if(at != -1)
			host = host.mid(0, at);
		QList<DomainMap::Target> targets = domainMap->entry(host);
		log_debug("%s has %d routes", qPrintable(host), targets.count());
		QByteArray str = "http://" + targets[0].first.toUtf8() + ':' + QByteArray::number(targets[0].second) + mr->path();
		//QUrl url(QByteArray("http://localhost:80/static/chat.js") /*+ req->path()*/);
		QUrl url(str);
		firstRead = true;
		zr->start(mr->method(), url, mr->headers());
		zr->endBody();
	}

public slots:
	void mr_readyRead()
	{
		QByteArray buf = mr->read();
		log_debug("got chunk: %d", buf.size());
		zr->writeBody(buf);
	}

	void mr_finished()
	{
		log_debug("finished");
		zr->endBody();
	}

	void zr_readyRead()
	{
		log_debug("zr_readyRead");

		if(firstRead)
		{
			firstRead = false;

			HttpHeaders headers = zr->responseHeaders();

			if(instructTypes.contains(headers.get("Content-Type")))
			{
				ad.rids += mr->rid();
				ad.request.method = mr->method();
				ad.request.path = mr->path();

				ad.haveResponse = true;
				ad.response.code = zr->responseCode();
				ad.response.status = zr->responseStatus();
				ad.response.headers = headers;
				ad.response.body = zr->readResponseBody();

				instruct = true;
			}
			else
			{
				resp = mr->createResponse();
				connect(resp, SIGNAL(finished()), SLOT(resp_finished()));

				if(!headers.contains("Content-Length"))
				{
					if(!headers.contains("Transfer-Encoding"))
						headers += HttpHeader("Transfer-Encoding", "chunked");
				}

				resp->start(zr->responseCode(), zr->responseStatus(), headers);
				QByteArray buf = zr->readResponseBody();
				if(!buf.isEmpty())
					resp->write(buf);
			}
		}
		else
		{
			QByteArray buf = zr->readResponseBody();
			if(!buf.isEmpty())
			{
				if(instruct)
				{
					ad.response.body += buf;
				}
				else
				{
					log_debug("writing %d", buf.size());
					resp->write(buf);
				}
			}
		}

		if(zr->isFinished())
		{
			log_debug("zr isFinished");

			delete mr;
			delete zr;

			if(instruct)
			{
				emit q->finishedForAccept(ad);
			}
			else
			{
				resp->close();
			}
		}
	}

	void zr_bytesWritten(int count)
	{
		Q_UNUSED(count);
		log_debug("zr_bytesWritten");
	}

	void zr_error()
	{
		log_debug("zr_error");
	}

	void resp_finished()
	{
		log_debug("resp_finished");

		delete resp;
		emit q->finishedByPassthrough();
	}
};

ProxySession::ProxySession(ZurlManager *zurlManager, DomainMap *domainMap, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, zurlManager, domainMap);
}

ProxySession::~ProxySession()
{
	delete d;
}

void ProxySession::setInspectData(const InspectData &idata)
{
	d->haveInspectData = true;
	d->idata = idata;
}

void ProxySession::add(RequestSession *rs)
{
	d->add(rs);
}

#include "proxysession.moc"
