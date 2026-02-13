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

#ifndef PUBLISHLASTIDS_H
#define PUBLISHLASTIDS_H

#include <QString>
#include <QDateTime>
#include <QMap>
#include <QHash>

// Cache with LRU expiration
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
