/*
 * Copyright (C) 2019 Fanout, Inc.
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

#ifndef FORMAT_H
#define FORMAT_H

#include <QByteArray>
#include <QString>

namespace Format {

class Handler
{
public:
	virtual ~Handler() {}

	// Returns null array on error
	virtual QByteArray handle(char type, const QByteArray &arg, QString *error) const = 0;
};

QByteArray process(const QByteArray &format, Handler *handler, int *partialPos = 0, QString *error = 0);

}

#endif
