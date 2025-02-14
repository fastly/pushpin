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
#include <assert.h>

static thread_local DeferCall *g_instance = nullptr;

DeferCall::DeferCall() = default;

DeferCall::~DeferCall() = default;

void DeferCall::defer(std::function<void ()> handler)
{
	Call c;
	c.handler = handler;

	deferredCalls_.push_back(c);

	QMetaObject::invokeMethod(this, "callNext", Qt::QueuedConnection);
}

DeferCall *DeferCall::global()
{
	if(!g_instance)
		g_instance = new DeferCall;

	return g_instance;
}

void DeferCall::cleanup()
{
	delete g_instance;
	g_instance = nullptr;
}

void DeferCall::callNext()
{
	// there can't be more invokeMethod resolutions than queued calls
	assert(!deferredCalls_.empty());

	Call c = deferredCalls_.front();
	deferredCalls_.pop_front();

	c.handler();
}
