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

#ifndef DOMAINMAP_H
#define DOMAINMAP_H

#include <QPair>
#include <QString>

// this class offers fast access to the domains file. the table is maintained
//   by a background thread so that file access doesn't cause blocking.

class DomainMap
{
public:
	typedef QPair<QString, int> Target;

	DomainMap(const QString &fileName);
	~DomainMap();

	QList<Target> entry(const QString &domain) const;

private:
	class Private;
	Private *d;

	class Thread;
	class Worker;
};

#endif
