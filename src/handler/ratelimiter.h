/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#ifndef RATELIMITER_H
#define RATELIMITER_H

#include <memory>
#include <QObject>

class RateLimiter : public QObject
{
	Q_OBJECT

public:
	class Action
	{
	public:
		virtual ~Action() {}

		virtual bool execute() = 0;
	};

	RateLimiter();
	~RateLimiter();

	void setRate(int actionsPerSecond);
	void setHwm(int hwm);
	void setBatchWaitEnabled(bool on);

	bool addAction(const QString &key, Action *action, int weight = 1);
	Action *lastAction(const QString &key) const;

private:
	class Private;
	std::shared_ptr<Private> d;
};

#endif
