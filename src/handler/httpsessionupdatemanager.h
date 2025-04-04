/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
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
 *
 * $FANOUT_END_LICENSE$
 */

#ifndef HTTPSESSIONUPDATEMANAGER_H
#define HTTPSESSIONUPDATEMANAGER_H

#define TIMERS_PER_UNIQUE_UPDATE_REGISTRATION 1

class QUrl;
class HttpSession;

class HttpSessionUpdateManager
{
public:
	HttpSessionUpdateManager();
	~HttpSessionUpdateManager();

	// no-op if session already registered and resetTimeout=false
	void registerSession(HttpSession *hs, int timeout, const QUrl &uri, bool resetTimeout = false);

	void unregisterSession(HttpSession *hs);

private:
	class Private;
	Private *d;
};

#endif
