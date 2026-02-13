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

#include "ratelimiter.h"

#include <QList>
#include <QMap>
#include "timer.h"
#include "defercall.h"

#define MIN_BATCH_INTERVAL 25

class RateLimiter::Private
{
public:
	class ActionItem
	{
	public:
		Action *action;
		int weight;

		ActionItem(Action *_action = 0, int _weight = 0) :
			action(_action),
			weight(_weight)
		{
		}
	};

	class Bucket
	{
	public:
		QList<ActionItem> actions;
		int weight;
		int debt;

		Bucket() :
			weight(0),
			debt(0)
		{
		}

		~Bucket()
		{
			foreach(const ActionItem &i, actions)
				delete i.action;
		}
	};

	RateLimiter *q;
	int rate;
	int hwm;
	bool batchWaitEnabled;
	QMap<QString, Bucket> buckets;
	QString lastKey;
	std::unique_ptr<Timer> timer;
	bool firstPass;
	int batchInterval;
	int batchSize;
	bool lastBatchEmpty;

	Private(RateLimiter *_q) :
		q(_q),
		rate(-1),
		hwm(-1),
		batchWaitEnabled(false),
		batchInterval(-1),
		batchSize(-1),
		lastBatchEmpty(false)
	{
		timer = std::make_unique<Timer>();
		timer->timeout.connect(boost::bind(&Private::timeout, this));
	}

	void setRate(int actionsPerSecond)
	{
		if(actionsPerSecond > 0)
		{
			rate = actionsPerSecond;

			if(rate >= 1000 / MIN_BATCH_INTERVAL)
			{
				batchInterval = MIN_BATCH_INTERVAL;
				batchSize = (rate * batchInterval + 999) / 1000;
			}
			else
			{
				batchInterval = 1000 / rate;
				batchSize = 1;
			}
		}
		else
		{
			rate = -1;
			batchInterval = -1;
			batchSize = -1;
		}

		setup();
	}

	bool addAction(const QString &key, int weight, Action *action)
	{
		Bucket &bucket = buckets[key];
		if(hwm > 0 && bucket.weight + weight > hwm)
			return false;

		bucket.actions += ActionItem(action, weight);
		bucket.weight += weight;

		setup();
		return true;
	}

private:
	void setup()
	{
		if(rate > 0)
		{
			if(!buckets.isEmpty() || !lastBatchEmpty)
			{
				if(timer->isActive())
				{
					// After the first pass, switch to batch interval
					if(!firstPass)
						timer->setInterval(batchInterval);
				}
				else
				{
					// Process first batch
					firstPass = true;

					if(batchWaitEnabled)
					{
						// If wait enabled, collect for awhile before processing
						timer->start(batchInterval);
					}
					else
					{
						// If wait not enabled, process immediately
						timer->start(0);
					}
				}
			}
			else
			{
				if(lastBatchEmpty)
				{
					// If we processed nothing on this pass, stop timer
					lastBatchEmpty = false;
					timer->stop();
				}
			}
		}
		else
		{
			if(!buckets.isEmpty())
			{
				if(timer->isActive())
				{
					// Ensure we're on fastest interval
					timer->setInterval(0);
				}
				else
				{
					// Process first batch right away
					firstPass = true;
					timer->start(0);
				}
			}
			else
			{
				timer->stop();
			}
		}
	}

	// Return false if self destroyed
	bool processBatch()
	{
		if(buckets.isEmpty())
		{
			lastBatchEmpty = true;
			return true;
		}

		lastBatchEmpty = false;

		QMap<QString, Bucket>::iterator it;

		if(!lastKey.isNull())
		{
			it = buckets.find(lastKey);

			if(it == buckets.end())
				it = buckets.begin();
		}
		else
		{
			it = buckets.begin();
		}

		std::weak_ptr<Private> self = q->d;

		int processed = 0;
		while((batchSize < 1 || processed < batchSize) && it != buckets.end())
		{
			Bucket &bucket = it.value();

			QString key = it.key();

			if(bucket.debt <= 0)
			{
				ActionItem ai = bucket.actions.takeFirst();
				Action *action = ai.action;
				int weight = ai.weight;

				bucket.weight -= weight;

				bool ret = action->execute();
				delete action;

				if(self.expired())
					return false;

				if(ret)
				{
					if(weight > 1)
						processed += weight;
					else
						++processed;

					if(batchSize >= 1 && processed > batchSize)
					{
						bucket.debt += processed - batchSize;
					}
				}
			}
			else
			{
				--bucket.debt;
				++processed;
			}

			if(bucket.actions.isEmpty() && bucket.debt <= 0)
			{
				lastKey = key;
				it = buckets.erase(it);
			}
			else
			{
				++it;
				if(it == buckets.end())
					it = buckets.begin();
			}
		}

		if(it != buckets.end())
			lastKey = it.key();

		return true;
	}

	void timeout()
	{
		if(!processBatch())
			return;

		firstPass = false;

		setup();
	}
};

RateLimiter::RateLimiter()
{
	d = std::make_shared<Private>(this);
}

RateLimiter::~RateLimiter() = default;

void RateLimiter::setRate(int actionsPerSecond)
{
	d->setRate(actionsPerSecond);
}

void RateLimiter::setHwm(int hwm)
{
	d->hwm = hwm;
}

void RateLimiter::setBatchWaitEnabled(bool on)
{
	d->batchWaitEnabled = on;
}

bool RateLimiter::addAction(const QString &key, Action *action, int weight)
{
	return d->addAction(key, weight, action);
}

RateLimiter::Action *RateLimiter::lastAction(const QString &key) const
{
	if(d->buckets.contains(key))
	{
		const Private::Bucket &bucket = d->buckets[key];
		if(!bucket.actions.isEmpty())
			return bucket.actions.last().action;
	}

	return 0;
}
