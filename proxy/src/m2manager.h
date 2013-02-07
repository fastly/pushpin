/*
 * Copyright (C) 2012-2013 Fan Out Networks, Inc.
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

#ifndef M2MANAGER_H
#define M2MANAGER_H

#include <QObject>
#include "m2request.h"

class M2ResponsePacket;
class M2Response;

class M2Manager : public QObject
{
	Q_OBJECT

public:
	M2Manager(QObject *parent = 0);
	~M2Manager();

	bool setIncomingPlainSpecs(const QStringList &specs);
	bool setIncomingHttpsSpecs(const QStringList &specs);
	bool setOutgoingSpecs(const QStringList &specs);

	M2Request *takeNext();
	M2Response *createResponse(const M2Request::Rid &rid);

signals:
	void requestReady();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class M2Request;
	friend class M2Response;
	void unlink(M2Request *req);
	void unlink(M2Response *resp);
	void writeResponse(const M2ResponsePacket &packet);
};

#endif
