/*
 * Copyright (C) 2016 Fanout, Inc.
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

#ifndef PUBLISHITEM_H
#define PUBLISHITEM_H

#include <QString>
#include <QHash>
#include <QVariant>
#include "publishformat.h"

class PublishItem
{
public:
	QString channel;
	QString id;
	QString prevId;
	QHash<PublishFormat::Type, PublishFormat> formats;
	QHash<QString, QString> meta;
	int size;
	bool noSeq;

	PublishFormat format; // for single format items

	PublishItem() :
		size(-1),
		noSeq(false)
	{
	}

	static PublishItem fromVariant(const QVariant &vitem, const QString &channel = QString(), bool *ok = 0, QString *errorMessage = 0);
};

#endif
