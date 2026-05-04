/*
 * Copyright (C) 2012-2016 Fanout, Inc.
 * Copyright (C) 2026 Fastly, Inc.
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

#ifndef LOG_H
#define LOG_H

#include "rust/bindings.h"
#include <QString>

enum LogLevel {
    LOG_LEVEL_ERROR = ffi::LOG_LEVEL_ERROR,
    LOG_LEVEL_WARNING = ffi::LOG_LEVEL_WARN,
    LOG_LEVEL_INFO = ffi::LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG = ffi::LOG_LEVEL_DEBUG,
    LOG_LEVEL_TRACE = ffi::LOG_LEVEL_TRACE,
};

bool log_init(const QString &outputFile = QString());

int log_outputLevel();
void log_setOutputLevel(int level);
bool log_rotate(const QString &outputFile);

void log(int level, const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_warning(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_debug(const char *fmt, ...);

// Log without prefixing or anything. Useful for forwarding log data
void log_raw(const char *line);

#endif
