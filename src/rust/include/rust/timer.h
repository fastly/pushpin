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

#ifndef RUST_TIMER_H
#define RUST_TIMER_H

extern "C"
{
	struct ExpiredTimer
	{
		int key;
		size_t user_data;
	};

	void *timer_wheel_create(unsigned int capacity);
	void timer_wheel_destroy(void *wheel);
	int timer_add(void *wheel, quint64 expires, size_t user_data);
	void timer_remove(void *wheel, int key);
	qint64 timer_wheel_timeout(void *wheel);
	void timer_wheel_update(void *wheel, quint64 curtime);
	ExpiredTimer timer_wheel_take_expired(void *wheel);
}

#endif
