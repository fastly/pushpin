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

#include "publishlastids.h"

#include <assert.h>

PublishLastIds::PublishLastIds(int maxCapacity) :
	maxCapacity_(maxCapacity)
{
}

void PublishLastIds::set(const QString &channel, const QString &id)
{
	QDateTime now = QDateTime::currentDateTimeUtc();

	if(table_.contains(channel))
	{
		Item &i = table_[channel];
		recentlyUsed_.remove(TimeStringPair(i.time, channel));
		i.id = id;
		i.time = now;
		recentlyUsed_.insert(TimeStringPair(i.time, channel), i);
	}
	else
	{
		while(!table_.isEmpty() && table_.count() >= maxCapacity_)
		{
			// remove oldest
			QMutableMapIterator<TimeStringPair, Item> it(recentlyUsed_);
			assert(it.hasNext());
			it.next();
			QString channel = it.value().channel;
			it.remove();
			table_.remove(channel);
		}

		Item i;
		i.channel = channel;
		i.id = id;
		i.time = now;
		table_.insert(channel, i);
		recentlyUsed_.insert(TimeStringPair(i.time, channel), i);
	}
}

void PublishLastIds::remove(const QString &channel)
{
	if(table_.contains(channel))
	{
		Item &i = table_[channel];
		recentlyUsed_.remove(TimeStringPair(i.time, channel));
		table_.remove(channel);
	}
}

void PublishLastIds::clear()
{
	recentlyUsed_.clear();
	table_.clear();
}

QString PublishLastIds::value(const QString &channel)
{
	return table_.value(channel).id;
}
