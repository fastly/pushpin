/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifndef ZHTTPREQUEST_H
#define ZHTTPREQUEST_H

#include <QObject>
#include "httpheaders.h"

class QUrl;

class ZhttpResponsePacket;
class ZhttpManager;

class ZhttpRequest : public QObject
{
	Q_OBJECT

public:
	enum ErrorCondition
	{
		ErrorGeneric,
		ErrorPolicy,
		ErrorConnect,
		ErrorConnectTimeout,
		ErrorTls,
		ErrorLengthRequired,
		ErrorTimeout,
		ErrorUnavailable
	};

	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	~ZhttpRequest();

	Rid rid() const;

	void setConnectHost(const QString &host);
	void setIgnorePolicies(bool on);
	void setIgnoreTlsErrors(bool on);

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

	friend class ZhttpManager;
	ZhttpRequest(QObject *parent = 0);
	void setup(ZhttpManager *manager);
	void handle(const ZhttpResponsePacket &packet);
};

#endif
