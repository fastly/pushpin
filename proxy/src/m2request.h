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

#ifndef M2REQUEST_H
#define M2REQUEST_H

#include <QObject>
#include "packet/httpheaders.h"

class M2RequestPacket;
class M2Manager;
class M2Response;

class M2Request : public QObject
{
	Q_OBJECT

public:
	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	~M2Request();

	Rid rid() const;
	bool isHttps() const;
	bool isFinished() const;

	QString method() const;
	QByteArray path() const;
	const HttpHeaders & headers() const;

	QByteArray read(int size = -1);

	// for streamed input, this is updated as body data is received
	int actualContentLength() const;

	// make a response object using same manager and rid
	M2Response *createResponse();

signals:
	void readyRead();
	void finished();
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class M2Manager;
	M2Request(QObject *parent = 0);
	bool handle(M2Manager *manager, const M2RequestPacket &packet, bool https);
	void activate();
	void uploadDone();
};

#endif
