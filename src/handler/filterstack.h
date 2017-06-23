/*
 * Copyright (C) 2017 Fanout, Inc.
 *
 * This file is part of Pushpin.
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
 */

#ifndef FILTERSTACK_H
#define FILTERSTACK_H

#include <QStringList>
#include "filter.h"

class FilterStack : public Filter
{
public:
	FilterStack(const Filter::Context &context, const QStringList &filters);

	// takes ownership of filters in list
	FilterStack(const Filter::Context &context, const QList<Filter*> &filters);

	~FilterStack();

	// reimplemented
	virtual SendAction sendAction() const;
	virtual QByteArray update(const QByteArray &data);
	virtual QByteArray finalize();

private:
	QList<Filter*> filters_;
};

#endif
