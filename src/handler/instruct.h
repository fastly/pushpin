/*
 * Copyright (C) 2016-2019 Fanout, Inc.
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

#ifndef INSTRUCT_H
#define INSTRUCT_H

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QList>
#include <QHash>
#include <QUrl>
#include "packet/httpresponsedata.h"

class Instruct
{
public:
	enum HoldMode
	{
		NoHold,
		ResponseHold,
		StreamHold
	};

	enum KeepAliveMode
	{
		NoKeepAlive,
		Idle,
		Interval
	};

	class Channel
	{
	public:
		QString name;
		QString prevId;
		QStringList filters;
	};

	HoldMode holdMode;
	QList<Channel> channels;
	int timeout;
	QList<QByteArray> exposeHeaders;
	KeepAliveMode keepAliveMode;
	QByteArray keepAliveData;
	int keepAliveTimeout;
	QHash<QString, QString> meta;
	HttpResponseData response;
	QUrl nextLink;
	int nextLinkTimeout;
	QUrl goneLink;

	Instruct() :
		holdMode(NoHold),
		timeout(-1),
		keepAliveMode(NoKeepAlive),
		keepAliveTimeout(-1),
		nextLinkTimeout(-1)
	{
	}

	static Instruct fromResponse(const HttpResponseData &response, bool *ok = 0, QString *errorMessage = 0);
};

#endif
