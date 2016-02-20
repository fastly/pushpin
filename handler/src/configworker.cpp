/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "configworker.h"

#include <QJsonDocument>
#include <QJsonObject>
#include "httpserver.h"
#include "controlrequest.h"

ConfigWorker::ConfigWorker(HttpRequest *req, const QByteArray &responseContentType, ZrpcManager *proxyControlClient, const QString &targetHost, int targetPort, bool targetSsl, bool targetOverHttp, QObject *parent) :
	Deferred(parent),
	req_(req)
{
	req_->setParent(this);
	responseContentType_ = responseContentType;
	proxyControlClient_ = proxyControlClient;
	targetHost_ = targetHost;
	targetPort_ = targetPort;
	targetSsl_ = targetSsl;
	targetOverHttp_ = targetOverHttp;

	// first clear all routes
	Deferred *d = ControlRequest::routeRemoveAll(proxyControlClient_, this);
	connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(proxyRouteRemoveAll_finished(const DeferredResult &)));
}

void ConfigWorker::respondError()
{
	req_->respond(500, "Internal Server Error", "Failed to apply configuration.\n");
	setFinished(true);
}

void ConfigWorker::proxyRouteRemoveAll_finished(const DeferredResult &result)
{
	if(result.success)
	{
		// now set route
		Deferred *d = ControlRequest::routeSet(proxyControlClient_, targetHost_, targetPort_, targetSsl_, targetOverHttp_, this);
		connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(proxyRouteSet_finished(const DeferredResult &)));
	}
	else
	{
		respondError();
	}
}

void ConfigWorker::proxyRouteSet_finished(const DeferredResult &result)
{
	if(result.success)
	{
		QString message = "Updated";
		if(responseContentType_ == "application/json")
		{
			QVariantMap obj;
			obj["message"] = message;
			QString body = QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(obj)).toJson(QJsonDocument::Compact));
			req_->respond(200, "OK", responseContentType_, body + "\n");
		}
		else // text/plain
		{
			req_->respond(200, "OK", message + "\n");
		}

		setFinished(true);
	}
	else
	{
		respondError();
	}
}
