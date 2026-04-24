/*
 * Copyright (C) 2016 Fanout, Inc.
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

#ifndef VARIANTUTIL_H
#define VARIANTUTIL_H

#include "variant.h"

namespace VariantUtil {

void setSuccess(bool *ok, QString *errorMessage);
void setError(bool *ok, QString *errorMessage, const QString &msg);

bool isKeyedObject(const Variant &in);
Variant createSameKeyedObject(const Variant &in);
bool keyedObjectIsEmpty(const Variant &in);
bool keyedObjectContains(const Variant &in, const QString &name);
Variant keyedObjectGetValue(const Variant &in, const QString &name);
void keyedObjectInsert(Variant *in, const QString &name, const Variant &value);

Variant getChild(const Variant &in, const QString &parentName, const QString &childName,
                 bool required, bool *ok = 0, QString *errorMessage = 0);
Variant getKeyedObject(const Variant &in, const QString &parentName, const QString &childName,
                       bool required, bool *ok = 0, QString *errorMessage = 0);
VariantList getList(const Variant &in, const QString &parentName, const QString &childName,
                    bool required, bool *ok = 0, QString *errorMessage = 0);
QString getString(const Variant &in, bool *ok = 0);
QString getString(const Variant &in, const QString &parentName, const QString &childName,
                  bool required, bool *ok = 0, QString *errorMessage = 0);

// Return true if item modified
bool convertToJsonStyleInPlace(Variant *in);

Variant convertToJsonStyle(const Variant &in);

} // namespace VariantUtil

#endif
