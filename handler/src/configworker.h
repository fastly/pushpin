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

#ifndef CONFIGWORKER_H
#define CONFIGWORKER_H

#include <QByteArray>
#include "deferred.h"

class ZrpcManager;
class HttpRequest;

class ConfigWorker : public Deferred
{
	Q_OBJECT

public:
	ConfigWorker(HttpRequest *req, ZrpcManager *proxyControlClient, const QString &targetHost, int targetPort, bool targetSsl, bool targetOverHttp, QObject *parent = 0);

private:
	HttpRequest *req_;
	ZrpcManager *proxyControlClient_;
	QString targetHost_;
	int targetPort_;
	bool targetSsl_;
	bool targetOverHttp_;

	void respondError();

private slots:
	void proxyRouteRemoveAll_finished(const DeferredResult &result);
	void proxyRouteSet_finished(const DeferredResult &result);
};

#endif
