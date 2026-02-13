/*
 * Copyright (C) 2017 Fanout, Inc.
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

#ifndef IDFORMAT_H
#define IDFORMAT_H

#include <QByteArray>
#include <QString>
#include <QHash>

namespace IdFormat {

class ContentRenderer
{
public:
	ContentRenderer(const QByteArray &defaultId, bool hex);

	// Return null array on error
	QByteArray update(const QByteArray &data);
	QByteArray finalize();

	QString errorMessage() { return errorMessage_; }

	QByteArray process(const QByteArray &data);

private:
	QByteArray defaultId_;
	bool hex_;
	QByteArray buf_;
	QString errorMessage_;
};

QByteArray renderId(const QByteArray &data, const QHash<QString, QByteArray> &vars, QString *error = 0);

}

#endif
