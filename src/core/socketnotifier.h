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

#ifndef SOCKETNOTIFIER_H
#define SOCKETNOTIFIER_H

#include <QSocketNotifier>
#include <boost/signals2.hpp>

class EventLoop;

class SocketNotifier : public QObject
{
	Q_OBJECT

public:
	enum Interest
	{
		Read = 0x01,
		Write = 0x02,
	};

	SocketNotifier(int socket, uint8_t interest);
	~SocketNotifier();

	bool isReadEnabled() const { return readEnabled_; }
	bool isWriteEnabled() const { return writeEnabled_; }
	int socket() const { return socket_; }

	void setReadEnabled(bool enable);
	void setWriteEnabled(bool enable);

	boost::signals2::signal<void(int, uint8_t)> activated;

private slots:
	void innerReadActivated(int socket);
	void innerWriteActivated(int socket);

private:
	int socket_;
	uint8_t interest_;
	bool readEnabled_;
	bool writeEnabled_;
	QSocketNotifier *readInner_;
	QSocketNotifier *writeInner_;
	EventLoop *loop_;
	int regId_;

	static void cb_fd_activated(void *ctx, uint8_t readiness);
	void fd_activated(uint8_t readiness);
};

#endif
