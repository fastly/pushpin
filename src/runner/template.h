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

#ifndef TEMPLATE_H
#define TEMPLATE_H

#include <QString>
#include <QVariantMap>

namespace Template {

QString render(const QString &content, const QVariantMap &context, QString *error = 0);
bool renderFile(const QString &inFile, const QString &outFile, const QVariantMap &context, QString *error = 0);

void dumpTemplate(const QString &content);

}

#endif
