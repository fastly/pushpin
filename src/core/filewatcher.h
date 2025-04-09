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

#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#include <boost/signals2.hpp>
#include "socketnotifier.h"
#include "rust/bindings.h"

class QString;

class FileWatcher
{
public:
	FileWatcher();
	~FileWatcher();

	void start(const QString &filePath);

	boost::signals2::signal<void()> fileChanged;

private:
	ffi::FileWatcher *inner_;
	std::unique_ptr<SocketNotifier> sn_;

	void sn_activated(int socket, uint8_t readiness);
};

#endif
