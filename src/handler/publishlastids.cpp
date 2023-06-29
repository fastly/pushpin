/*
 * Copyright (C) 2016 Fanout, Inc.
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
