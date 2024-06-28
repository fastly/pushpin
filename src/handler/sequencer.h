/*
 * Copyright (C) 2016-2021 Fanout, Inc.
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

#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <QObject>
#include <boost/signals2.hpp>

class PublishLastIds;
class PublishItem;

class Sequencer : public QObject
{
	Q_OBJECT

public:
	Sequencer(PublishLastIds *publishLastIds, QObject *parent = 0);
	~Sequencer();

	void setWaitMax(int msecs);
	void setIdCacheTtl(int secs);

	// seq = false means ID cache handling only
	// note: may emit signals
	void addItem(const PublishItem &item, bool seq = true);

	void clearPendingForChannel(const QString &channel);

	boost::signals2::signal<void(const PublishItem&)> itemReady;

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
