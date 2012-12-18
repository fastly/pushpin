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

#ifndef ZURLREQUEST_H
#define ZURLREQUEST_H

#include <QObject>
#include "packet/httpheaders.h"

class QUrl;

class ZurlResponsePacket;
class ZurlManager;

class ZurlRequest : public QObject
{
	Q_OBJECT

public:
	enum ErrorCondition
	{
		ErrorGeneric,
		ErrorPolicy,
		ErrorConnect,
		ErrorTls,
		ErrorLengthRequired,
		ErrorTimeout,
		ErrorUnavailable
	};

	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	~ZurlRequest();

	Rid rid() const;

	void setConnectHost(const QString &host);

	void start(const QString &method, const QUrl &url, const HttpHeaders &headers);

	// may call this multiple times
	void writeBody(const QByteArray &body);

	void endBody();

	int bytesAvailable() const;
	bool isFinished() const;
	ErrorCondition errorCondition() const;

	int responseCode() const;
	QByteArray responseStatus() const;
	HttpHeaders responseHeaders() const;

	QByteArray readResponseBody(int size = -1); // takes from the buffer

signals:
	void readyRead();
	void bytesWritten(int count);
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZurlManager;
	ZurlRequest(QObject *parent = 0);
	void setup(ZurlManager *manager);
	void handle(const ZurlResponsePacket &packet);
};

#endif
