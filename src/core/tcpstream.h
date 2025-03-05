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
#include "readwrite.h"

class SocketNotifier;

class TcpStream : public ReadWrite
{
public:
	~TcpStream();

	// reimplemented
	virtual QByteArray read(int size = -1);
	virtual int write(const QByteArray &buf);
	virtual int errorCondition() const { return errorCondition_; }

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
