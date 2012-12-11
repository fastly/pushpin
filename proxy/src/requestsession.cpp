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

#include "requestsession.h"

#include "packet/httprequestdata.h"
#include "m2request.h"
#include "inspectdata.h"
#include "inspectmanager.h"
#include "inspectrequest.h"

class RequestSession::Private : public QObject
{
	Q_OBJECT

public:
	RequestSession *q;
	InspectManager *inspectManager;
	M2Request *m2Request;
	InspectRequest *inspectRequest;

	Private(RequestSession *_q, InspectManager *_inspectManager) :
		QObject(_q),
		q(_q),
		inspectManager(_inspectManager),
		m2Request(0),
		inspectRequest(0)
	{
	}

public slots:
	void m2Request_error()
	{
		// TODO
	}

	void inspectRequest_finished(const InspectData &idata)
	{
		emit q->inspectFinished(idata);
	}

	void inspectRequest_error()
	{
		// default action is to proxy without sharing
		InspectData idata;
		idata.doProxy = true;
		emit q->inspectFinished(idata);
	}
};

RequestSession::RequestSession(InspectManager *inspectManager, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, inspectManager);
}

RequestSession::~RequestSession()
{
	delete d;
}

M2Request *RequestSession::request()
{
	return d->m2Request;
}

void RequestSession::start(M2Request *req)
{
	d->m2Request = req;
	connect(d->m2Request, SIGNAL(error()), d, SLOT(m2Request_error()));

	d->inspectRequest = d->inspectManager->createRequest();
	connect(d->inspectRequest, SIGNAL(finished(const InspectData &)), d, SLOT(inspectRequest_finished(const InspectData &)));
	connect(d->inspectRequest, SIGNAL(error()), d, SLOT(inspectRequest_error()));

	HttpRequestData hdata;
	hdata.method = req->method();
	hdata.path = req->path();
	hdata.headers = req->headers();
	d->inspectRequest->start(hdata);
}

#include "requestsession.moc"
