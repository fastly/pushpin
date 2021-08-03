/*
 * Copyright (C) 2021 Fanout, Inc.
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

#include "timerwheel.h"

#include "rust/timer.h"

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
