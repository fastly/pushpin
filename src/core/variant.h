/*
 * Copyright (C) 2026 Fastly, Inc.
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

#ifndef VARIANT_H
#define VARIANT_H

#include "qtcompat.h"
#include <QHash>
#include <QList>
#include <QMap>
#include <QVariant>

using Variant = QVariant;
using VariantHash = QVariantHash;
using VariantMap = QVariantMap;
using VariantList = QVariantList;

// Note: typeId() and canConvert() functions are already provided by qtcompat.h
// Since Variant = QVariant (alias), those functions work directly with our
// types

#endif
