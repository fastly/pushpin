/*
 * Copyright (C) 2015 Fanout, Inc.
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

#include "statusreasons.h"

#include "rust/bindings.h"
#include <QByteArray>
#include <limits>

namespace StatusReasons {

QByteArray getReason(int code) {
    uint16_t c = static_cast<uint16_t>(
        std::clamp(code, static_cast<int>(std::numeric_limits<uint16_t>::min()),
                   static_cast<int>(std::numeric_limits<uint16_t>::max())));

    const char *s = ffi::statusreasons_get_reason(c);
    qsizetype size = qstrlen(s);

    // SAFETY: s has a static lifetime
    return QByteArray::fromRawData(s, size);
}

} // namespace StatusReasons
