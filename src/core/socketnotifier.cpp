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
	readiness_(0),
	loop_(EventLoop::instance()),
	regId_(-1)
{
	assert((interest & Read) || (interest & Write));

	// start by assuming ready
	readiness_ = interest;

	if(loop_)
	{
		// if the rust-based eventloop is available, use it

		uint8_t einterest = 0;

		if(interest & Read)
			einterest |= EventLoop::Readable;

		if(interest & Write)
			einterest |= EventLoop::Writable;

		regId_ = loop_->registerFd(socket_, einterest, SocketNotifier::cb_fd_activated, this);
		assert(regId_ >= 0);
	}
	else
	{
		// else fall back to qt eventloop

		if(interest & Read)
		{
			readInner_ = new QSocketNotifier(socket, QSocketNotifier::Read);
			connect(readInner_, &QSocketNotifier::activated, this, &SocketNotifier::innerReadActivated);

			// start out disabled. will enable when initial readiness cleared
			readInner_->setEnabled(false);
		}

		if(interest & Write)
		{
			writeInner_ = new QSocketNotifier(socket, QSocketNotifier::Write);
			connect(writeInner_, &QSocketNotifier::activated, this, &SocketNotifier::innerWriteActivated);

			// start out disabled. will enable when initial readiness cleared
			writeInner_->setEnabled(false);
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
}

void SocketNotifier::setWriteEnabled(bool enable)
{
	writeEnabled_ = enable;
}

void SocketNotifier::clearReadiness(uint8_t readiness)
{
	readiness_ &= ~readiness;

	if(readInner_ && !(readiness_ & Read))
		readInner_->setEnabled(true);

	if(writeInner_ && !(readiness_ & Write))
		writeInner_->setEnabled(true);
}

void SocketNotifier::innerReadActivated(int socket)
{
	Q_UNUSED(socket);

	// QSocketNotifier is level-triggered. disable until readiness cleared
	readInner_->setEnabled(false);

	apply(Read);
}

void SocketNotifier::innerWriteActivated(int socket)
{
	Q_UNUSED(socket);

	// QSocketNotifier is level-triggered. disable until readiness cleared
	writeInner_->setEnabled(false);

	apply(Write);
}

void SocketNotifier::apply(uint8_t readiness)
{
	// calculate which bits went from 0->1
	uint8_t changes = readiness & ~readiness_;

	readiness_ |= readiness;

	if((readEnabled_ && (changes & Read)) || (writeEnabled_ && (changes & Write)))
		activated(socket_, changes);
}

void SocketNotifier::cb_fd_activated(void *ctx, uint8_t ereadiness)
{
	SocketNotifier *self = (SocketNotifier *)ctx;

	self->fd_activated(ereadiness);
}

void SocketNotifier::fd_activated(uint8_t ereadiness)
{
	uint8_t readiness = 0;

	if(ereadiness & EventLoop::Readable)
		readiness |= Read;

	if(ereadiness & EventLoop::Writable)
		readiness |= Write;

	if(readiness)
		apply(readiness);
}
