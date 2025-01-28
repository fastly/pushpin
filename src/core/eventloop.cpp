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

thread_local EventLoop *g_instance = nullptr;

EventLoop::EventLoop(int capacity) :
    inner_(ffi::event_loop_create(capacity))
{
    // only one per thread allowed
    assert(!g_instance);

    g_instance = this;
}

EventLoop::~EventLoop()
{
    ffi::event_loop_destroy(inner_);

    g_instance = nullptr;
}

int EventLoop::exec()
{
    return ffi::event_loop_exec(inner_);
}

void EventLoop::exit(int code)
{
    ffi::event_loop_exit(inner_, code);
}

int EventLoop::registerFd(int fd, unsigned char interest, void (*cb)(void *), void *ctx)
{
    size_t id;

    if(ffi::event_loop_register_fd(inner_, fd, interest, cb, ctx, &id) != 0)
        return -1;

    return (int)id;
}

int EventLoop::registerTimer(int timeout, void (*cb)(void *), void *ctx)
{
    size_t id;

    if(ffi::event_loop_register_timer(inner_, timeout, cb, ctx, &id) != 0)
        return -1;

    return (int)id;
}

void EventLoop::deregister(int id)
{
    ffi::event_loop_deregister(inner_, id);
}

EventLoop *EventLoop::instance()
{
    return g_instance;
}