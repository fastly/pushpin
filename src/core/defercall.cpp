/*
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

#include "defercall.h"

#include <QThread>
#include <QMetaObject>
#include <boost/signals2.hpp>
#include "timer.h"
#include "event.h"
#include "eventloop.h"

namespace {

class ThreadWake : public QObject
{
	Q_OBJECT

public:
	ThreadWake() :
		wakeQueued_(false)
	{
		EventLoop *loop = EventLoop::instance();

		if(loop)
		{
			auto [regId, sr] = loop->registerCustom(ThreadWake::cb_ready, this);
			assert(regId >= 0);

			sr_ = std::move(sr);
		}
	}

	// requests the awake signal to be emitted from the object's event loop
	// at the next opportunity. this is safe to call from another thread. if
	// this is called multiple times before the signal is emitted, the signal
	// will only be emitted once.
	void wake()
	{
		std::lock_guard<std::mutex> guard(mutex_);

		if(wakeQueued_)
			return;

		wakeQueued_ = true;

		if(sr_)
			sr_->setReadiness(Event::Readable);
		else
			QMetaObject::invokeMethod(this, "slotReady", Qt::QueuedConnection);
	}

	boost::signals2::signal<void()> awake;

private slots:
	void slotReady()
	{
		ready();
	}

private:
	std::unique_ptr<Event::SetReadiness> sr_;
	std::mutex mutex_;
	bool wakeQueued_;

	static void cb_ready(void *ctx, uint8_t readiness)
	{
		Q_UNUSED(readiness);

		ThreadWake *self = (ThreadWake *)ctx;

		self->ready();
	}

	void ready()
	{
		{
			std::lock_guard<std::mutex> guard(mutex_);

			wakeQueued_ = false;
		}

		awake();
	}
};

}

class DeferCall::Manager
{
public:
	Manager() :
		thread_(QThread::currentThread())
	{
		timer_.setSingleShot(true);
		timer_.timeout.connect(boost::bind(&Manager::timer_timeout, this));

		threadWake_.awake.connect(boost::bind(&Manager::threadWake_awake, this));
	}

	void add(const std::weak_ptr<Call> &c)
	{
		std::lock_guard<std::mutex> guard(callsMutex_);

		calls_.push_back(c);

		if(QThread::currentThread() == thread_)
		{
			if(!timer_.isActive())
				timer_.start(0);
		}
		else
		{
			threadWake_.wake();
		}
	}

	void flush()
	{
		while(!isCallsEmpty())
			process();
	}

private:
	QThread *thread_;
	Timer timer_;
	ThreadWake threadWake_;
	std::mutex callsMutex_;
	std::list<std::weak_ptr<Call>> calls_;

	// thread-safe
	bool isCallsEmpty()
	{
		std::lock_guard<std::mutex> guard(callsMutex_);

		return calls_.empty();
	}

	// thread-safe
	void process()
	{
		std::list<std::weak_ptr<Call>> ready;

		// lock to take list
		{
			std::lock_guard<std::mutex> guard(callsMutex_);

			// process all calls queued so far, but not any that may get queued
			// during processing
			ready.swap(calls_);
		}

		// process list while not locked
		for(auto c : ready)
		{
			if(auto p = c.lock())
			{
				auto source = p->source.lock();

				// if call is valid then its source will be too
				assert(source);

				source->erase(p->sourceElement);

				p->handler();
			}
		}
	}

	void timer_timeout()
	{
		process();

		// no need to re-arm the timer. if new calls were queued during
		// processing, add() will have taken care of that
	}

	void threadWake_awake()
	{
		process();
	}
};

std::list<std::shared_ptr<DeferCall::Call>>::size_type DeferCall::CallsList::size() const
{
	std::lock_guard<std::mutex> guard(mutex);

	return l.size();
}

std::list<std::shared_ptr<DeferCall::Call>>::iterator DeferCall::CallsList::append(const std::shared_ptr<DeferCall::Call> &c)
{
	std::lock_guard<std::mutex> guard(mutex);

	l.push_back(c);

	// get an iterator to the element that was pushed
	auto it = l.end();
	--it;

	return it;
}

void DeferCall::CallsList::erase(std::list<std::shared_ptr<DeferCall::Call>>::iterator position)
{
	std::lock_guard<std::mutex> guard(mutex);

	l.erase(position);
}

DeferCall::DeferCall() :
	thread_(QThread::currentThread()),
	deferredCalls_(std::make_shared<CallsList>())
{
	if(!localManager)
	{
		localManager = std::make_shared<Manager>();

		std::lock_guard<std::mutex> guard(managerByThreadMutex);
		managerByThread[thread_] = localManager;
	}
}

DeferCall::~DeferCall() = default;

void DeferCall::defer(std::function<void ()> handler)
{
	std::shared_ptr<Call> c = std::make_shared<Call>();
	c->handler = handler;
	c->source = deferredCalls_;
	c->sourceElement = deferredCalls_->append(c);

	Manager *manager = localManager.get();

	if(QThread::currentThread() != thread_)
	{
		std::lock_guard<std::mutex> guard(managerByThreadMutex);
		auto it = managerByThread.find(thread_);
		assert(it != managerByThread.end());

		manager = it->second.get();
	}

	// manager keeps a weak pointer, so we can invalidate pending calls by
	// simply deleting them
	manager->add(c);
}

DeferCall *DeferCall::global()
{
	if(!localInstance)
		localInstance = std::make_unique<DeferCall>();

	return localInstance.get();
}

void DeferCall::cleanup()
{
	if(localManager)
		localManager->flush();

	localInstance.reset();

	if(localManager)
	{
		std::lock_guard<std::mutex> guard(managerByThreadMutex);
		managerByThread.erase(QThread::currentThread());

		localManager.reset();
	}
}

thread_local std::shared_ptr<DeferCall::Manager> DeferCall::localManager = std::shared_ptr<DeferCall::Manager>();
thread_local std::unique_ptr<DeferCall> DeferCall::localInstance = std::unique_ptr<DeferCall>();

std::unordered_map<QThread*, std::shared_ptr<DeferCall::Manager>> DeferCall::managerByThread = std::unordered_map<QThread*, std::shared_ptr<DeferCall::Manager>>();
std::mutex DeferCall::managerByThreadMutex = std::mutex();

#include "defercall.moc"
