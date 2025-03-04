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

#include "tcpstream.h"

#include "socketnotifier.h"

#define DEFAULT_READ_SIZE 16384

TcpStream::TcpStream(ffi::TcpStream *inner) :
	inner_(inner),
	errorCondition_(0),
	alive_(std::make_shared<std::monostate>(std::monostate{}))
{
	int fd = ffi::tcp_stream_as_raw_fd(inner_);

	sn_ = std::make_unique<SocketNotifier>(fd, SocketNotifier::Read | SocketNotifier::Write);
	sn_->activated.connect(boost::bind(&TcpStream::sn_activated, this, boost::placeholders::_1, boost::placeholders::_2));
	sn_->setReadEnabled(true);
	sn_->setWriteEnabled(true);
}

TcpStream::~TcpStream()
{
	ffi::tcp_stream_destroy(inner_);
}

QByteArray TcpStream::read(int size)
{
	if(size < 0)
		size = DEFAULT_READ_SIZE;

	QByteArray buf(size, 0);
	errorCondition_ = 0;

	int ret = ffi::tcp_stream_read(inner_, (uint8_t *)buf.data(), buf.size(), &errorCondition_);

	// re-enable the notifier. we always do this regardless of the outcome of
	// the read, in case the notifier is edge-triggered. if the notifier is
	// level-triggered and there are still bytes to read, this re-enabling
	// will cause readability to be signaled again, but only once
	sn_->setReadEnabled(true);

	if(ret < 0)
		return QByteArray();

	buf.resize(ret);

	return buf;
}

int TcpStream::write(const QByteArray &buf)
{
	errorCondition_ = 0;

	int ret = ffi::tcp_stream_write(inner_, (const uint8_t *)buf.constData(), buf.size(), &errorCondition_);

	// re-enable the notifier. we always do this regardless of the outcome of
	// the write, in case the notifier is edge-triggered. if the notifier is
	// level-triggered and the socket is able to accept more bytes, this
	// re-enabling will cause writability to be signaled again, but only once
	sn_->setWriteEnabled(true);

	if(ret < 0)
		return -1;

	return ret;
}

void TcpStream::sn_activated(int socket, uint8_t readiness)
{
	Q_UNUSED(socket);

	// in case notifier is level-triggered, disable in order to avoid
	// repeated signaling. will turn it back on after a read is attempted
	if(readiness & SocketNotifier::Read)
		sn_->setReadEnabled(false);

	// in case notifier is level-triggered, disable in order to avoid
	// repeated signaling. will turn it back on after a write is attempted
	if(readiness & SocketNotifier::Write)
		sn_->setWriteEnabled(false);

	std::weak_ptr<std::monostate> self = alive_;

	if(readiness & SocketNotifier::Read)
		readReady();

	if(self.expired())
		return;

	if(readiness & SocketNotifier::Write)
		writeReady();
}
