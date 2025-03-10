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

#include <assert.h>
#include <QHostAddress>
#include "socketnotifier.h"

#define DEFAULT_READ_SIZE 16384

TcpStream::TcpStream() :
	inner_(nullptr),
	errorCondition_(0)
{
}

TcpStream::TcpStream(ffi::TcpStream *inner) :
	inner_(inner),
	errorCondition_(0),
	alive_(std::make_shared<std::monostate>(std::monostate{}))
{
	setupNotifier();
}

TcpStream::~TcpStream()
{
	reset();
}

bool TcpStream::connect(const QHostAddress &addr, quint16 port)
{
	reset();

	QByteArray ip = addr.toString().toUtf8();
	errorCondition_ = 0;

	int e;
	inner_ = ffi::tcp_stream_connect(ip.data(), port, &e);
	if(!inner_)
	{
		errorCondition_ = e;
		return false;
	}

	setupNotifier();

	return true;
}

bool TcpStream::checkConnected()
{
	assert(inner_);

	errorCondition_ = 0;

	int e;
	if(ffi::tcp_stream_check_connected(inner_, &e) != 0)
	{
		errorCondition_ = e;
		return false;
	}

	return true;
}

QByteArray TcpStream::read(int size)
{
	assert(inner_);

	if(size < 0)
		size = DEFAULT_READ_SIZE;

	QByteArray buf(size, 0);
	errorCondition_ = 0;

	int ret = ffi::tcp_stream_read(inner_, (uint8_t *)buf.data(), buf.size(), &errorCondition_);

	if(ret < 0)
	{
		if(errorCondition_ == EAGAIN)
			sn_->clearReadiness(SocketNotifier::Read);

		return QByteArray();
	}

	buf.resize(ret);

	return buf;
}

int TcpStream::write(const QByteArray &buf)
{
	assert(inner_);

	errorCondition_ = 0;

	int ret = ffi::tcp_stream_write(inner_, (const uint8_t *)buf.constData(), buf.size(), &errorCondition_);

	if(ret < 0)
	{
		if(errorCondition_ == EAGAIN)
			sn_->clearReadiness(SocketNotifier::Write);

		return -1;
	}

	return ret;
}

void TcpStream::reset()
{
	sn_.reset();

	if(inner_)
	{
		ffi::tcp_stream_destroy(inner_);
		inner_ = nullptr;
	}
}

void TcpStream::setupNotifier()
{
	int fd = ffi::tcp_stream_as_raw_fd(inner_);

	sn_ = std::make_unique<SocketNotifier>(fd, SocketNotifier::Read | SocketNotifier::Write);
	sn_->activated.connect(boost::bind(&TcpStream::sn_activated, this, boost::placeholders::_1, boost::placeholders::_2));
	sn_->setReadEnabled(true);
	sn_->setWriteEnabled(true);
}

void TcpStream::sn_activated(int socket, uint8_t readiness)
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
