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

#include "defercall.h"
#include "eventloop.h"

SocketNotifier::SocketNotifier(int socket, Type type) :
	socket_(socket),
	type_(type),
	enabled_(true),
	inner_(nullptr),
	loop_(EventLoop::instance()),
	regId_(-1)
{
	if(loop_)
	{
		// if the rust-based eventloop is available, use it

		unsigned char interest = 0;
		switch(type_)
		{
			case SocketNotifier::Read:
				interest = EventLoop::Readable;
				break;
			case SocketNotifier::Write:
				interest = EventLoop::Writable;
				break;
		}

		regId_ = loop_->registerFd(socket_, interest, SocketNotifier::cb_fd_activated, this);
	}
	else
	{
		// else fall back to qt eventloop

		QSocketNotifier::Type qType = type == Read ? QSocketNotifier::Read : QSocketNotifier::Write;

		inner_ = new QSocketNotifier(socket, qType);
		connect(inner_, &QSocketNotifier::activated, this, &SocketNotifier::innerActivated);
	}
}

SocketNotifier::~SocketNotifier()
{
	if(inner_)
	{
		inner_->setEnabled(false);

		inner_->disconnect(this);
		inner_->setParent(0);
		DeferCall::deleteLater(inner_);
	}

	if(regId_ >= 0)
		loop_->deregister(regId_);
}

void SocketNotifier::setEnabled(bool enable)
{
	enabled_ = enable;

	if(inner_)
		inner_->setEnabled(enabled_);
}

void SocketNotifier::innerActivated(int socket)
{
	activated(socket);
}

void SocketNotifier::cb_fd_activated(void *ctx)
{
	SocketNotifier *self = (SocketNotifier *)ctx;

	self->fd_activated();
}

void SocketNotifier::fd_activated()
{
	if(enabled_)
		activated(socket_);
}
