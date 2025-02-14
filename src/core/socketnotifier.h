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
	enum Type
	{
		Read = 1,
		Write = 2,
	};

	SocketNotifier(int socket, Type type);
	~SocketNotifier();

	bool isEnabled() const { return enabled_; }
	int socket() const { return socket_; }
	Type type() const { return type_; }

	void setEnabled(bool enable);

	boost::signals2::signal<void(int)> activated;

private slots:
	void innerActivated(int socket);

private:
	int socket_;
	Type type_;
	bool enabled_;
	QSocketNotifier *inner_;
	EventLoop *loop_;
	int regId_;

	static void cb_fd_activated(void *ctx);
	void fd_activated();
};

#endif
