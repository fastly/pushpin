/*
 * Copyright (C) 2021 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#ifndef TIMER_H
#define TIMER_H

#include <qobject.h>
#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;

class TimerManager;

class Timer : public QObject
{
	Q_OBJECT

public:
	Timer();
	~Timer();

	bool isActive() const;

	void setSingleShot(bool singleShot);
	void setInterval(int msec);
	void start(int msec);
	void start();
	void stop();

	// initialization is thread local
	static void init(int capacity);

	// only call if there are no active timers
	static void deinit();

	Signal timeout;

private:
	friend class TimerManager;

	bool singleShot_;
	int interval_;
	int timerId_;

	void timerReady();
};

#endif
