/*
 * Copyright (C) 2016 Fanout, Inc.
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

#ifndef PUBLISHLASTIDS_H
#define PUBLISHLASTIDS_H

#include <QString>
#include <QDateTime>
#include <QMap>
#include <QHash>

// cache with LRU expiration
class PublishLastIds
{
public:
	PublishLastIds(int maxCapacity);
	void set(const QString &channel, const QString &id);
	void remove(const QString &channel);
	void clear();
	QString value(const QString &channel);

private:
	typedef QPair<QDateTime, QString> TimeStringPair;

	class Item
	{
	public:
		QString channel;
		QString id;
		QDateTime time;
	};

	QHash<QString, Item> table_;
	QMap<TimeStringPair, Item> recentlyUsed_;
	int maxCapacity_;
};

#endif
