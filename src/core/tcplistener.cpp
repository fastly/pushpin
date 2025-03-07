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

#include "tcplistener.h"

#include <assert.h>
#include "socketnotifier.h"

TcpListener::TcpListener() :
	inner_(nullptr),
	errorCondition_(0)
{
}

TcpListener::~TcpListener()
{
	reset();
}

bool TcpListener::bind(const QHostAddress &addr, quint16 port)
{
	reset();

	QByteArray ip = addr.toString().toUtf8();
	errorCondition_ = 0;

	inner_ = ffi::tcp_listener_bind(ip.data(), port, &errorCondition_);
	if(!inner_)
		return false;

	int fd = ffi::tcp_listener_as_raw_fd(inner_);

	sn_ = std::make_unique<SocketNotifier>(fd, SocketNotifier::Read);
	sn_->activated.connect(boost::bind(&TcpListener::sn_activated, this));
	sn_->setReadEnabled(true);

	return true;
}

std::tuple<QHostAddress, quint16> TcpListener::localAddress() const
{
	assert(inner_);

	QByteArray ip(256, 0);
	size_t ip_size = ip.size();
	quint16 port;
	if(ffi::tcp_listener_local_addr(inner_, ip.data(), &ip_size, &port) != 0)
		return {QHostAddress(), 0};

	ip.resize(ip_size);
	QHostAddress addr(QString::fromUtf8(ip));

	return {addr, port};
}

std::unique_ptr<TcpStream> TcpListener::accept()
{
	assert(inner_);

	errorCondition_ = 0;

	ffi::TcpStream *s_inner = ffi::tcp_listener_accept(inner_, &errorCondition_);
	if(!s_inner)
	{
		if(errorCondition_ == EAGAIN)
			sn_->clearReadiness(SocketNotifier::Read);

		return std::unique_ptr<TcpStream>(); // null
	}

	TcpStream *s = new TcpStream(s_inner);

	return std::unique_ptr<TcpStream>(s);
}

void TcpListener::reset()
{
	sn_.reset();

	if(inner_)
	{
		ffi::tcp_listener_destroy(inner_);
		inner_ = nullptr;
	}
}

void TcpListener::sn_activated()
{
	streamsReady();
}
