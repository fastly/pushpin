/*
 * Copyright (C) 2012-2022 Fanout, Inc.
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

#include "log.h"

#include <stdarg.h>

static void log(int level, const char *fmt, va_list ap) {
    QString str = QString::vasprintf(fmt, ap);
    ffi::log_log(level, str.toUtf8().data());
}

bool log_init(const QString &outputFile) {
    int ret;

    if (!outputFile.isEmpty())
        ret = ffi::log_init(outputFile.toUtf8().data());
    else
        ret = ffi::log_init(nullptr);

    return (ret == 0);
}

int log_outputLevel() { return ffi::log_get_level(); }

void log_setOutputLevel(int level) { ffi::log_set_level(level); }

bool log_rotate(const QString &outputFile) {
    return (ffi::log_rotate(outputFile.toUtf8().data()) == 0);
}

void log(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log(level, fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log(LOG_LEVEL_ERROR, fmt, ap);
    va_end(ap);
}

void log_warning(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log(LOG_LEVEL_WARNING, fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log(LOG_LEVEL_INFO, fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log(LOG_LEVEL_DEBUG, fmt, ap);
    va_end(ap);
}

void log_raw(const char *s) { ffi::log_log_raw(s); }
