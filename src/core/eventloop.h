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

#include "rust/bindings.h"

class EventLoop
{
public:
	enum Interest
	{
		Readable = ffi::READABLE,
		Writable = ffi::WRITABLE,
	};

	EventLoop(int capacity);
	~EventLoop();

	int exec();
	void exit(int code);

	int registerFd(int fd, unsigned char interest, void (*cb)(void *), void *ctx);
	int registerTimer(int timeout, void (*cb)(void *), void *ctx);
	void deregister(int id);

	static EventLoop *instance();

private:
	ffi::EventLoopRaw *inner_;
};

#endif
