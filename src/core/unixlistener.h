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

#ifndef UNIXLISTENER_H
#define UNIXLISTENER_H

#include <memory>
#include <QHostAddress>
#include <boost/signals2.hpp>
#include "rust/bindings.h"
#include "unixstream.h"

class SocketNotifier;

class UnixListener
{
public:
	UnixListener();
	~UnixListener();

	// disable copying
	UnixListener(const UnixListener &) = delete;
	UnixListener & operator=(const UnixListener &) = delete;

	bool bind(const QString &path);
	std::unique_ptr<UnixStream> accept();
	int errorCondition() const { return errorCondition_; }

	boost::signals2::signal<void()> streamsReady;

private:
	ffi::UnixListener *inner_;
	std::unique_ptr<SocketNotifier> sn_;
	int errorCondition_;

	void reset();
	void sn_activated();
};

#endif
