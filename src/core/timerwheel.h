/*
 * Copyright (C) 2021 Fanout, Inc.
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

#ifndef TIMERWHEEL_H
#define TIMERWHEEL_H

#include <QPair>

namespace ffi {
	struct TimerWheel;
}

class TimerWheel
{
public:
	class Expired
	{
	public:
		int key; // <0 if invalid
		size_t userData;
	};

	TimerWheel(int capacity);
	~TimerWheel();

	// disable copying
	TimerWheel(const TimerWheel &) = delete;
	TimerWheel & operator=(const TimerWheel &) = delete;

	// returns <0 if no capacity
	int add(quint64 expires, size_t userData);

	void remove(int key);

	// returns <0 if no timers
	qint64 timeout() const;

	void update(quint64 curtime);

	Expired takeExpired();

private:
	ffi::TimerWheel *raw_;
};

#endif
