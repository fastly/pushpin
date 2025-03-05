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

#include "socketnotifier.h"

#include <assert.h>
#include "defercall.h"
#include "eventloop.h"

SocketNotifier::SocketNotifier(int socket, uint8_t interest) :
	socket_(socket),
	interest_(interest),
	readEnabled_(true),
	writeEnabled_(true),
	readInner_(nullptr),
	writeInner_(nullptr),
	loop_(EventLoop::instance()),
	regId_(-1)
{
	assert((interest & SocketNotifier::Read) || (interest & SocketNotifier::Write));

	if(loop_)
	{
		// if the rust-based eventloop is available, use it

		uint8_t einterest = 0;

		if(interest & SocketNotifier::Read)
			einterest |= EventLoop::Readable;

		if(interest & SocketNotifier::Write)
			einterest |= EventLoop::Writable;

		regId_ = loop_->registerFd(socket_, einterest, SocketNotifier::cb_fd_activated, this);
		assert(regId_ >= 0);
	}
	else
	{
		// else fall back to qt eventloop

		if(interest & SocketNotifier::Read)
		{
			readInner_ = new QSocketNotifier(socket, QSocketNotifier::Read);
			connect(readInner_, &QSocketNotifier::activated, this, &SocketNotifier::innerReadActivated);
		}

		if(interest & SocketNotifier::Write)
		{
			writeInner_ = new QSocketNotifier(socket, QSocketNotifier::Write);
			connect(writeInner_, &QSocketNotifier::activated, this, &SocketNotifier::innerWriteActivated);
		}
	}
}

SocketNotifier::~SocketNotifier()
{
	if(readInner_)
	{
		readInner_->setEnabled(false);

		readInner_->disconnect(this);
		readInner_->setParent(0);
		DeferCall::deleteLater(readInner_);
	}

	if(writeInner_)
	{
		writeInner_->setEnabled(false);

		writeInner_->disconnect(this);
		writeInner_->setParent(0);
		DeferCall::deleteLater(writeInner_);
	}

	if(regId_ >= 0)
		loop_->deregister(regId_);
}

void SocketNotifier::setReadEnabled(bool enable)
{
	readEnabled_ = enable;

	if(readInner_)
		readInner_->setEnabled(readEnabled_);
}

void SocketNotifier::setWriteEnabled(bool enable)
{
	writeEnabled_ = enable;

	if(writeInner_)
		writeInner_->setEnabled(writeEnabled_);
}

void SocketNotifier::innerReadActivated(int socket)
{
	activated(socket, SocketNotifier::Read);
}

void SocketNotifier::innerWriteActivated(int socket)
{
	activated(socket, SocketNotifier::Write);
}

void SocketNotifier::cb_fd_activated(void *ctx, uint8_t readiness)
{
	SocketNotifier *self = (SocketNotifier *)ctx;

	self->fd_activated(readiness);
}

void SocketNotifier::fd_activated(uint8_t ereadiness)
{
	uint8_t readiness = 0;

	if(readEnabled_ && (ereadiness & EventLoop::Readable))
		readiness |= SocketNotifier::Read;
	if(writeEnabled_ && (ereadiness & EventLoop::Writable))
		readiness |= SocketNotifier::Write;

	if(readiness)
		activated(socket_, readiness);
}
