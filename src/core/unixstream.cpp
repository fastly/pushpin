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

#include "unixstream.h"

#include "socketnotifier.h"

#define DEFAULT_READ_SIZE 16384

UnixStream::UnixStream() :
	inner_(nullptr),
	errorCondition_(0)
{
}

UnixStream::UnixStream(ffi::UnixStream *inner) :
	inner_(inner),
	errorCondition_(0),
	alive_(std::make_shared<std::monostate>(std::monostate{}))
{
	setupNotifier();
}

UnixStream::~UnixStream()
{
	reset();
}

bool UnixStream::connect(const QString &path)
{
	reset();

	QByteArray p = path.toUtf8();
	errorCondition_ = 0;

	inner_ = ffi::unix_stream_connect(p.data(), &errorCondition_);
	if(!inner_)
		return false;

	setupNotifier();

	return true;
}

bool UnixStream::checkConnected()
{
	assert(inner_);

	errorCondition_ = 0;

	if(ffi::unix_stream_check_connected(inner_, &errorCondition_) != 0)
		return false;

	return true;
}

QByteArray UnixStream::read(int size)
{
	assert(inner_);

	if(size < 0)
		size = DEFAULT_READ_SIZE;

	QByteArray buf(size, 0);
	errorCondition_ = 0;

	int ret = ffi::unix_stream_read(inner_, (uint8_t *)buf.data(), buf.size(), &errorCondition_);

	if(ret < 0)
	{
		if(errorCondition_ == EAGAIN)
			sn_->clearReadiness(SocketNotifier::Read);

		return QByteArray();
	}

	buf.resize(ret);

	return buf;
}

int UnixStream::write(const QByteArray &buf)
{
	assert(inner_);

	errorCondition_ = 0;

	int ret = ffi::unix_stream_write(inner_, (const uint8_t *)buf.constData(), buf.size(), &errorCondition_);

	if(ret < 0)
	{
		if(errorCondition_ == EAGAIN)
			sn_->clearReadiness(SocketNotifier::Write);

		return -1;
	}

	return ret;
}

void UnixStream::reset()
{
	sn_.reset();

	if(inner_)
	{
		ffi::unix_stream_destroy(inner_);
		inner_ = nullptr;
	}
}

void UnixStream::setupNotifier()
{
	int fd = ffi::unix_stream_as_raw_fd(inner_);

	sn_ = std::make_unique<SocketNotifier>(fd, SocketNotifier::Read | SocketNotifier::Write);
	sn_->activated.connect(boost::bind(&UnixStream::sn_activated, this, boost::placeholders::_1, boost::placeholders::_2));
	sn_->setReadEnabled(true);
	sn_->setWriteEnabled(true);
}

void UnixStream::sn_activated(int socket, uint8_t readiness)
{
	Q_UNUSED(socket);

	std::weak_ptr<std::monostate> self = alive_;

	if(readiness & SocketNotifier::Read)
		readReady();

	if(self.expired())
		return;

	if(readiness & SocketNotifier::Write)
		writeReady();
}
