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

#ifndef ACSTRING_H
#define ACSTRING_H

#include <QString>
#include "acbytearray.h"

class AcString
{
public:
    AcString()
    {
        // ensure layout is transparent
        static_assert(sizeof(AcString) == sizeof(QString));
        static_assert(offsetof(AcString, inner_) == 0);
    }

    AcString(const QString &other) : inner_(other) {}

    AcByteArray toUtf8() const { return inner_.toUtf8(); }

    const QString & asQString() const { return inner_; }
    QString & asQString() { return inner_; }

private:
    QString inner_;
};

#endif
