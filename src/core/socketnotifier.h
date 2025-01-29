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

	bool isEnabled() const { return inner_->isEnabled(); }
	int socket() const { return inner_->socket(); }
	Type type() const { return inner_->type() == QSocketNotifier::Read ? Read : Write; }

	void setEnabled(bool enable);

	boost::signals2::signal<void(int)> activated;

private slots:
	void innerActivated(int socket);

private:
	QSocketNotifier *inner_;
};

#endif
