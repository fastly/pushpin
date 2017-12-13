/*
 * Copyright (C) 2014 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
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
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#ifndef ZROUTES_H
#define ZROUTES_H

#include <QObject>
#include "zhttpmanager.h"
#include "domainmap.h"

class ZRoutes : public QObject
{
	Q_OBJECT

public:
	ZRoutes(QObject *parent = 0);
	~ZRoutes();

	void setInstanceId(const QByteArray &id);
	void setDefaultOutSpecs(const QStringList &specs);
	void setDefaultOutStreamSpecs(const QStringList &specs);
	void setDefaultInSpecs(const QStringList &specs);

	void setup(const QList<DomainMap::ZhttpRoute> &routes);

	ZhttpManager *defaultManager();
	ZhttpManager *managerForRoute(const DomainMap::ZhttpRoute &route);

	void addRef(ZhttpManager *zhttpManager);
	void removeRef(ZhttpManager *zhttpManager);

private:
	class Private;
	Private *d;
};

#endif
