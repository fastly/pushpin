/*
 * Copyright (C) 2017 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
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

	// return null array on error
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
