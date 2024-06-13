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

#include "timerwheel.h"

#include "rust/bindings.h"

TimerWheel::TimerWheel(int capacity)
{
	raw_ = timer_wheel_create(capacity);
}

TimerWheel::~TimerWheel()
{
	timer_wheel_destroy(raw_);
}

int TimerWheel::add(quint64 expires, size_t userData)
{
	return timer_add(raw_, expires, userData);
}

void TimerWheel::remove(int key)
{
	timer_remove(raw_, key);
}

qint64 TimerWheel::timeout() const
{
	return timer_wheel_timeout(raw_);
}

void TimerWheel::update(quint64 curtime)
{
	timer_wheel_update(raw_, curtime);
}

TimerWheel::Expired TimerWheel::takeExpired()
{
	ExpiredTimer ret = timer_wheel_take_expired(raw_);

	Expired expired;
	expired.key = ret.key;
	expired.userData = ret.user_data;

	return expired;
}
