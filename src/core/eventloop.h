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

#ifndef EVENTLOOP_H
#define EVENTLOOP_H

#include <memory>
#include <optional>
#include <functional>
#include <list>
#include "rust/bindings.h"
#include "event.h"

class EventLoop
{
public:
	EventLoop(int capacity);
	~EventLoop();

	// Disable copying
	EventLoop(const EventLoop &) = delete;
	EventLoop & operator=(const EventLoop &) = delete;

	std::optional<int> step();
	int exec();
	void exit(int code);

	int registerFd(int fd, uint8_t interest, void (*cb)(void *, uint8_t), void *ctx);
	int registerTimer(int timeout, void (*cb)(void *, uint8_t), void *ctx);
	std::tuple<int, std::unique_ptr<Event::SetReadiness>> registerCustom(void (*cb)(void *, uint8_t), void *ctx);
	void deregister(int id);

	void addCleanupHandler(void (*handler)(void *), void *ctx);
	void removeCleanupHandler(void (*handler)(void *), void *ctx);

	static EventLoop *instance();

	/// Returns a future that constructs an event loop and executes it
	/// asynchronously. `setup` is called just prior to executing, and `done`
	/// is called when the event loop exits. `setup` and `done` must point
	/// to functions that live as long as the returned future.
	static ffi::UnitFuture *task(int capacity, std::function<void ()> *setup, std::function<void (int)> *done);

private:
	class CleanupHandler
	{
	public:
		void (*handler)(void *);
		void *ctx;

		bool operator==(const CleanupHandler &other) const
		{
			return (other.handler == handler && other.ctx == ctx);
		}
	};

	ffi::EventLoopRaw *inner_;
	bool taskManaged_;
	std::list<CleanupHandler> cleanupHandlers_;

	EventLoop(ffi::EventLoopRaw *inner);
	static void setup_cb(void *ctx, const ffi::EventLoopRaw *l);
	static void done_cb(void *ctx, int code);
};

#endif
