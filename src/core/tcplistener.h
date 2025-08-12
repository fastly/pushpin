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

#ifndef TCPLISTENER_H
#define TCPLISTENER_H

#include <memory>
#include <QHostAddress>
#include <boost/signals2.hpp>
#include "rust/bindings.h"
#include "tcpstream.h"

class SocketNotifier;

class TcpListener
{
public:
	TcpListener();
	~TcpListener();

	// disable copying
	TcpListener(const TcpListener &) = delete;
	TcpListener & operator=(const TcpListener &) = delete;

	bool bind(const QHostAddress &addr, uint16_t port);
	std::tuple<QHostAddress, uint16_t> localAddress() const;
	std::unique_ptr<TcpStream> accept();
	int errorCondition() const { return errorCondition_; }

	boost::signals2::signal<void()> streamsReady;

private:
	ffi::TcpListener *inner_;
	std::unique_ptr<SocketNotifier> sn_;
	int errorCondition_;

	void reset();
	void sn_activated();
};

#endif
