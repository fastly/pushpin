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

#include "timer.h"

class DeferCall::Manager
{
public:
	Manager()
	{
		timer_.setSingleShot(true);
		timer_.timeout.connect(boost::bind(&Manager::timer_timeout, this));
	}

	void add(const std::weak_ptr<Call> &c)
	{
		calls_.push_back(c);

		if(!timer_.isActive())
			timer_.start(0);
	}

	void flush()
	{
		while(!calls_.empty())
			process();
	}

private:
	Timer timer_;
	std::list<std::weak_ptr<Call>> calls_;

	void process()
	{
		// process all calls queued so far, but not any that may get queued
		// during processing
		std::list<std::weak_ptr<Call>> ready;
		ready.swap(calls_);

		for(auto c : ready)
		{
			if(auto p = c.lock())
				p->handler();
		}
	}

	void timer_timeout()
	{
		process();

		// no need to re-arm the timer. if new calls were queued during
		// processing, add() will have taken care of that
	}
};

DeferCall::DeferCall() = default;

DeferCall::~DeferCall() = default;

void DeferCall::defer(std::function<void ()> handler)
{
	std::shared_ptr<Call> c = std::make_shared<Call>();
	c->handler = handler;

	deferredCalls_.push_back(c);

	if(!manager)
		manager = new Manager;

	// manager keeps a weak pointer, so we can invalidate pending calls by
	// simply deleting them
	manager->add(c);
}

DeferCall *DeferCall::global()
{
	if(!instance)
		instance = new DeferCall;

	return instance;
}

void DeferCall::cleanup()
{
	if(manager)
		manager->flush();

	delete instance;
	instance = nullptr;

	delete manager;
	manager = nullptr;
}

thread_local DeferCall::Manager *DeferCall::manager = nullptr;
thread_local DeferCall *DeferCall::instance = nullptr;
