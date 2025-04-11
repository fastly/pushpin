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

#include "filewatcher.h"

#include <assert.h>
#include <QString>

FileWatcher::FileWatcher() :
	inner_(nullptr)
{
}

FileWatcher::~FileWatcher()
{
	sn_.reset();

	if(inner_)
		ffi::file_watcher_destroy(inner_);
}

bool FileWatcher::start(const QString &filePath)
{
	assert(!inner_);

	inner_ = ffi::file_watcher_create(filePath.toUtf8().data());
	if(!inner_)
		return false;

	int fd = ffi::file_watcher_as_raw_fd(inner_);

	sn_ = std::make_unique<SocketNotifier>(fd, SocketNotifier::Read);
	sn_->activated.connect(boost::bind(&FileWatcher::sn_activated, this, boost::placeholders::_1, boost::placeholders::_2));
	sn_->clearReadiness(SocketNotifier::Read);
	sn_->setReadEnabled(true);

	// in case the socket was activated before registering the notifier
	if(ffi::file_watcher_file_changed(inner_))
		deferCall_.defer([=] { fileChanged(); });

	return true;
}

void FileWatcher::sn_activated(int socket, uint8_t readiness)
{
	Q_UNUSED(socket);
	Q_UNUSED(readiness);

	sn_->clearReadiness(SocketNotifier::Read);

	if(ffi::file_watcher_file_changed(inner_))
		fileChanged();
}
