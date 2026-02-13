/*
 * Copyright (C) 2025 Fastly, Inc.
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
 */

#include "eventloop.h"

#include <assert.h>

static thread_local EventLoop *g_instance = nullptr;

EventLoop::EventLoop(int capacity) :
	taskManaged_(false)
{
	// Only one per thread allowed
	assert(!g_instance);

	inner_ = ffi::event_loop_create(capacity);

	g_instance = this;
}

EventLoop::EventLoop(ffi::EventLoopRaw *inner) :
	inner_(inner),
	taskManaged_(true)
{
	// Instance is task-managed, meaning the task owns the inner handle and
	// will manage where g_instance points. The instance should not destroy
	// the inner handle nor modify g_instance.
}

EventLoop::~EventLoop()
{
	while(!cleanupHandlers_.empty())
	{
		CleanupHandler h = cleanupHandlers_.front();
		cleanupHandlers_.pop_front();

		h.handler(h.ctx);
	}

	if(!taskManaged_)
	{
		ffi::event_loop_destroy(inner_);

		g_instance = nullptr;
	}
}

std::optional<int> EventLoop::step()
{
	std::optional<int> code;

	int x;
	if(ffi::event_loop_step(inner_, &x) == 0)
		code = x;

	return code;
}

int EventLoop::exec()
{
	return ffi::event_loop_exec(inner_);
}

void EventLoop::exit(int code)
{
	ffi::event_loop_exit(inner_, code);
}

int EventLoop::registerFd(int fd, uint8_t interest, void (*cb)(void *, uint8_t), void *ctx)
{
	size_t id;

	if(ffi::event_loop_register_fd(inner_, fd, interest, cb, ctx, &id) != 0)
		return -1;

	return (int)id;
}

int EventLoop::registerTimer(int timeout, void (*cb)(void *, uint8_t), void *ctx)
{
	size_t id;

	if(ffi::event_loop_register_timer(inner_, timeout, cb, ctx, &id) != 0)
		return -1;

	return (int)id;
}

std::tuple<int, std::unique_ptr<Event::SetReadiness>> EventLoop::registerCustom(void (*cb)(void *, uint8_t), void *ctx)
{
	size_t id;
	ffi::SetReadiness *srRaw = nullptr;

	if(ffi::event_loop_register_custom(inner_, cb, ctx, &id, &srRaw) != 0)
		return std::tuple<int, std::unique_ptr<Event::SetReadiness>>();

	std::unique_ptr<Event::SetReadiness> sr(new Event::SetReadiness(srRaw));

	return std::tuple<int, std::unique_ptr<Event::SetReadiness>>({(int)id, std::move(sr)});
}

void EventLoop::deregister(int id)
{
	assert(ffi::event_loop_deregister(inner_, id) == 0);
}

void EventLoop::addCleanupHandler(void (*handler)(void *), void *ctx)
{
	CleanupHandler h;
	h.handler = handler;
	h.ctx = ctx;

	cleanupHandlers_.push_front(h);
}

void EventLoop::removeCleanupHandler(void (*handler)(void *), void *ctx)
{
	CleanupHandler h;
	h.handler = handler;
	h.ctx = ctx;

	cleanupHandlers_.remove(h);
}

EventLoop *EventLoop::instance()
{
	return g_instance;
}

void EventLoop::setup_cb(void *ctx, const ffi::EventLoopRaw *l)
{
	std::function<void ()> *setup = (std::function<void ()> *)ctx;

	// Only one per thread allowed
	assert(!g_instance);

	// The task provides a const pointer but the EventLoop class needs a
	// non-const pointer. However, we promise not to call any non-const
	// methods with it.
	ffi::EventLoopRaw *inner = (ffi::EventLoopRaw *)l;

	g_instance = new EventLoop(inner);

	(*setup)();
}

void EventLoop::done_cb(void *ctx, int code)
{
	std::function<void (int)> *done = (std::function<void (int)> *)ctx;

	(*done)(code);

	delete g_instance;
	g_instance = nullptr;
}

ffi::UnitFuture *EventLoop::task(int capacity, std::function<void ()> *setup, std::function<void (int)> *done)
{
	return ffi::event_loop_task(capacity, setup_cb, setup, done_cb, done);
}
