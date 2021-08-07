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

#include "rtimer.h"

#include <assert.h>
#include <QDateTime>
#include <QTimer>
#include "timerwheel.h"

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

class TimerManager : public QObject
{
	Q_OBJECT

public:
	TimerManager(int capacity, QObject *parent = 0);

	int add(int msec, RTimer *r);
	void remove(int key);

private slots:
	void t_timeout();

private:
	TimerWheel wheel_;
	qint64 startTime_;
	quint64 currentTicks_;
	QTimer *t_;

	void updateTimeout(qint64 currentTime);
};

TimerManager::TimerManager(int capacity, QObject *parent) :
	QObject(parent),
	wheel_(TimerWheel(capacity))
{
	startTime_ = QDateTime::currentMSecsSinceEpoch();
	currentTicks_ = 0;

	t_ = new QTimer(this);
	connect(t_, &QTimer::timeout, this, &TimerManager::t_timeout);
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

static TimerManager *g_manager = 0;

RTimer::RTimer(QObject *parent) :
	QObject(parent),
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

	emit timeout();
}

void RTimer::init(int capacity)
{
	assert(!g_manager);

	g_manager = new TimerManager(capacity);
}

#include "rtimer.moc"
