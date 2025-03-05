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

#include "unixlistener.h"

#include "socketnotifier.h"

UnixListener::UnixListener() :
	inner_(nullptr)
{
}

UnixListener::~UnixListener()
{
	reset();
}

bool UnixListener::bind(const QString &path)
{
	reset();

	QByteArray p = path.toUtf8();
	errorCondition_ = 0;

	inner_ = ffi::unix_listener_bind(p.data(), &errorCondition_);
	if(!inner_)
		return false;

	int fd = ffi::unix_listener_as_raw_fd(inner_);

	sn_ = std::make_unique<SocketNotifier>(fd, SocketNotifier::Read);
	sn_->activated.connect(boost::bind(&UnixListener::sn_activated, this));
	sn_->setReadEnabled(true);

	return true;
}

std::unique_ptr<UnixStream> UnixListener::accept()
{
	assert(inner_);

	errorCondition_ = 0;

	ffi::UnixStream *s_inner = ffi::unix_listener_accept(inner_, &errorCondition_);
	if(!s_inner)
	{
		if(errorCondition_ == EAGAIN)
			sn_->clearReadiness(SocketNotifier::Read);

		return std::unique_ptr<UnixStream>(); // null
	}

	UnixStream *s = new UnixStream(s_inner);

	return std::unique_ptr<UnixStream>(s);
}

void UnixListener::reset()
{
	sn_.reset();

	if(inner_)
	{
		ffi::unix_listener_destroy(inner_);
		inner_ = nullptr;
	}
}

void UnixListener::sn_activated()
{
	streamsReady();
}
