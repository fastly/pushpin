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

#include "rtimer.h"

#include <assert.h>
#include <QDateTime>
#include <QTimer>
#include "timerwheel.h"
#include <memory>

#define TICK_DURATION_MS 10
#define UPDATE_TICKS_MAX 1000
#define EXPIRES_PER_CYCLE_MAX 100

static qint64 durationToTicksRoundDown(qint64 msec)
{
	return msec / TICK_DURATION_MS;
}

static qint64 durationToTicksRoundUp(qint64 msec)
{
	return (msec + TICK_DURATION_MS - 1) / TICK_DURATION_MS;
}

static qint64 ticksToDuration(qint64 ticks)
{
	return ticks * TICK_DURATION_MS;
}

class TimerManager
{
public:
	TimerManager(int capacity);

	int add(int msec, RTimer *r);
	void remove(int key);

private:
	void t_timeout();

	TimerWheel wheel_;
	qint64 startTime_;
	quint64 currentTicks_;
	std::unique_ptr<QTimer> t_;

	void updateTimeout(qint64 currentTime);
};

TimerManager::TimerManager(int capacity) :
	wheel_(TimerWheel(capacity))
{
	startTime_ = QDateTime::currentMSecsSinceEpoch();
	currentTicks_ = 0;

	t_ = std::make_unique<QTimer>(this);
	QObject::connect(t_.get(), &QTimer::timeout, this, &TimerManager::t_timeout);
	t_->setSingleShot(true);
}

int TimerManager::add(int msec, RTimer *r)
{
	qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

	// expireTime must be >= startTime_
	qint64 expireTime = qMax(currentTime + msec, startTime_);

	qint64 expiresTicks = durationToTicksRoundUp(expireTime - startTime_);

	int id = wheel_.add(expiresTicks, (size_t)r);

	if(id >= 0)
	{
		updateTimeout(currentTime);
	}

	return id;
}

void TimerManager::remove(int key)
{
	wheel_.remove(key);

	qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

	updateTimeout(currentTime);
}

void TimerManager::t_timeout()
{
	qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

	// time must go forward
	if(currentTime > startTime_)
	{
		currentTicks_ = (quint64)durationToTicksRoundDown(currentTime - startTime_);

		wheel_.update(currentTicks_);
	}

	for(int i = 0; i < EXPIRES_PER_CYCLE_MAX; ++i)
	{
		TimerWheel::Expired expired = wheel_.takeExpired();

		if(expired.key < 0)
		{
			break;
		}

		RTimer *r = (RTimer *)expired.userData;

		r->timerReady();
	}

	updateTimeout(currentTime);
}

void TimerManager::updateTimeout(qint64 currentTime)
{
	qint64 timeoutTicks = wheel_.timeout();

	if(timeoutTicks >= 0)
	{
		// currentTime must be >= startTime_
		currentTime = qMax(currentTime, startTime_);

		quint64 currentTicks = (quint64)durationToTicksRoundDown(currentTime - startTime_);

		// time must go forward
		currentTicks = qMax(currentTicks, currentTicks_);

		qint64 ticksSinceWheelUpdate = (qint64)(currentTicks - currentTicks_);

		// reduce the timeout by the time already elapsed
		timeoutTicks = qMax(timeoutTicks - ticksSinceWheelUpdate, (qint64)0);

		// cap the timeout so the wheel is regularly updated
		qint64 maxTimeoutTicks = qMax(UPDATE_TICKS_MAX - ticksSinceWheelUpdate, (qint64)0);
		timeoutTicks = qMin(timeoutTicks, maxTimeoutTicks);

		int msec = ticksToDuration(timeoutTicks);

		t_->start(msec);
	}
	else
	{
		t_->stop();
	}
}

static std::unique_ptr<TimerManager> g_manager = nullptr;

RTimer::RTimer() :
	singleShot_(false),
	interval_(0),
	timerId_(-1)
{
}

RTimer::~RTimer()
{
	stop();
}

bool RTimer::isActive() const
{
	return (timerId_ >= 0);
}

void RTimer::setSingleShot(bool singleShot)
{
	singleShot_ = singleShot;
}

void RTimer::start(int msec)
{
	interval_ = msec;

	start();
}

void RTimer::start()
{
	// must call RTimer::init first
	assert(g_manager);

	stop();

	int id = g_manager->add(interval_, this);
	assert(id >= 0);

	timerId_ = id;
}

void RTimer::stop()
{
	if(timerId_ >= 0)
	{
		assert(g_manager);

		g_manager->remove(timerId_);

		timerId_ = -1;
	}
}

void RTimer::timerReady()
{
	timerId_ = -1;

	if(!singleShot_)
	{
		start();
	}

	timeout();
}

void RTimer::init(int capacity)
{
	assert(!g_manager);

	g_manager = std::make_unique<TimerManager>(capacity);
}

void RTimer::deinit()
{
	delete g_manager;
	g_manager = 0;
}
