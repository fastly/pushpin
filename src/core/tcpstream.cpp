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
	errorCondition_(0)
{
	int fd = ffi::tcp_stream_as_raw_fd(inner_);

	snRead_ = std::make_unique<SocketNotifier>(fd, SocketNotifier::Read);
	snRead_->activated.connect(boost::bind(&TcpStream::snRead_activated, this));
	snRead_->setEnabled(true);

	snWrite_ = std::make_unique<SocketNotifier>(fd, SocketNotifier::Write);
	snWrite_->activated.connect(boost::bind(&TcpStream::snWrite_activated, this));
	snWrite_->setEnabled(true);
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
	snRead_->setEnabled(true);

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
	snWrite_->setEnabled(true);

	if(ret < 0)
		return -1;

	return ret;
}

void TcpStream::snRead_activated()
{
	// in case notifier is level-triggered, disable in order to avoid
	// repeated signaling. will turn it back on after a read is attempted
	snRead_->setEnabled(false);

	readReady();
}

void TcpStream::snWrite_activated()
{
	// in case notifier is level-triggered, disable in order to avoid
	// repeated signaling. will turn it back on after a write is attempted
	snWrite_->setEnabled(false);

	writeReady();
}
