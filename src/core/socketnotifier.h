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

	// initializes notifier with interest and considers the socket ready for
	// the specified interest. readiness must be cleared via clearReadiness()
	// in order for the activated signal to be emitted. the expected way to
	// use this class is to initialize it, perform I/O until progress can no
	// longer be made, clear readiness, then await the signal.
	SocketNotifier(int socket, uint8_t interest);

	~SocketNotifier();

	bool isReadEnabled() const { return readEnabled_; }
	bool isWriteEnabled() const { return writeEnabled_; }
	int socket() const { return socket_; }

	void setReadEnabled(bool enable);
	void setWriteEnabled(bool enable);

	uint8_t readiness() const { return readiness_; }
	void clearReadiness(uint8_t readiness);

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
	uint8_t readiness_;
	EventLoop *loop_;
	int regId_;

	void apply(uint8_t readiness);
	static void cb_fd_activated(void *ctx, uint8_t readiness);
	void fd_activated(uint8_t readiness);
};

#endif
