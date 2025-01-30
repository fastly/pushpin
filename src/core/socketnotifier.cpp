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

SocketNotifier::SocketNotifier(int socket, Type type)
{
	QSocketNotifier::Type qType = type == Read ? QSocketNotifier::Read : QSocketNotifier::Write;

	inner_ = new QSocketNotifier(socket, qType);
	connect(inner_, &QSocketNotifier::activated, this, &SocketNotifier::innerActivated);
}

SocketNotifier::~SocketNotifier()
{
	inner_->setEnabled(false);

	inner_->disconnect(this);
	inner_->setParent(0);
	DeferCall::deleteLater(inner_);
}

void SocketNotifier::setEnabled(bool enable)
{
	inner_->setEnabled(enable);
}

void SocketNotifier::innerActivated(int socket)
{
	activated(socket);
}
