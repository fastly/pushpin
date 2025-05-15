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

#ifndef ACUTIL_H
#define ACUTIL_H

#include <string.h>
#include <QList>
#include "acbytearray.h"

namespace AcUtil {

inline const QList<AcByteArray> & to(const QList<QByteArray> &other)
{
    // SAFETY: AcByteArray and QByteArray have identical layouts
    return reinterpret_cast<const QList<AcByteArray> &>(other);
}

inline const QList<QByteArray> & from(const QList<AcByteArray> &other)
{
    // SAFETY: AcByteArray and QByteArray have identical layouts
    return reinterpret_cast<const QList<QByteArray> &>(other);
}

}

#endif
