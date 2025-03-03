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

#ifndef TCPSTREAM_H
#define TCPSTREAM_H

#include <memory>
#include <QByteArray>
#include <boost/signals2.hpp>
#include "rust/bindings.h"

class SocketNotifier;

class TcpStream
{
public:
	~TcpStream();

	// size < 0 means default read size
	// returns buffer of bytes read. null buffer means error. empty means end
	QByteArray read(int size = -1);

	// returns amount accepted, or -1 for error
	int write(const QByteArray &buf);

	// returns errno of latest operation
	int errorCondition() const { return errorCondition_; }

	boost::signals2::signal<void()> readReady;
	boost::signals2::signal<void()> writeReady;

private:
	friend class TcpListener;

	ffi::TcpStream *inner_;
	std::unique_ptr<SocketNotifier> snRead_, snWrite_;
	int errorCondition_;

	TcpStream(ffi::TcpStream *inner);
	void snRead_activated();
	void snWrite_activated();
};

#endif
