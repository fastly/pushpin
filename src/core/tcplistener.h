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
#include <QtGlobal>
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

	bool bind(const QHostAddress &addr, quint16 port);
	std::tuple<QHostAddress, quint16> localAddress() const;
	std::unique_ptr<TcpStream> accept();

	boost::signals2::signal<void()> streamsReady;

private:
	void reset();
	void sn_activated();

	ffi::TcpListener *inner_;
	std::unique_ptr<SocketNotifier> sn_;
};

#endif
