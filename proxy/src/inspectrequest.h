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

#ifndef INSPECTREQUEST_H
#define INSPECTREQUEST_H

#include <QObject>

class HttpRequestData;
class InspectData;
class InspectResponsePacket;
class InspectManager;

class InspectRequest : public QObject
{
	Q_OBJECT

public:
	enum ErrorCondition
	{
		ErrorUnavailable,
		ErrorTimeout
	};

	~InspectRequest();

	void start(const HttpRequestData &hdata);

	ErrorCondition errorCondition() const;

signals:
	void finished(const InspectData &idata);
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class InspectManager;
	InspectRequest(QObject *parent = 0);
	QByteArray id() const;
	void setup(InspectManager *manager);
	void handle(const InspectResponsePacket &packet);
};

#endif
