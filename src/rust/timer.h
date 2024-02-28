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

#ifndef RUST_TIMER_H
#define RUST_TIMER_H

#include <QtGlobal>

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
