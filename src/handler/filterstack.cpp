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

#include "filterstack.h"

FilterStack::FilterStack(const Filter::Context &context, const QStringList &filters)
{
	foreach(const QString &name, filters)
	{
		Filter *f = Filter::create(name);
		f->setContext(context);
		filters_ += f;
	}
}

FilterStack::FilterStack(const Filter::Context &context, const QList<Filter*> &filters)
{
	filters_ = filters;

	foreach(Filter *f, filters_)
		f->setContext(context);
}

FilterStack::~FilterStack()
{
	qDeleteAll(filters_);
}

Filter::SendAction FilterStack::sendAction() const
{
	foreach(Filter *f, filters_)
	{
		SendAction a = f->sendAction();
		if(a == Drop)
			return Drop;
	}

	return Send;
}

QByteArray FilterStack::update(const QByteArray &data)
{
	QByteArray buf = data;
	foreach(Filter *f, filters_)
	{
		buf = f->update(buf);
		if(buf.isNull())
		{
			setError(QString("%1: %2").arg(f->name(), f->errorMessage()));
			return QByteArray();
		}
	}
	return buf;
}

QByteArray FilterStack::finalize()
{
	QByteArray out("");
	foreach(Filter *f, filters_)
	{
		if(!out.isEmpty())
		{
			out = f->update(out);
			if(out.isNull())
			{
				setError(QString("%1: %2").arg(f->name(), f->errorMessage()));
				return QByteArray();
			}
		}

		QByteArray buf = f->finalize();
		if(buf.isNull())
		{
			setError(QString("%1: %2").arg(f->name(), f->errorMessage()));
			return QByteArray();
		}

		out += buf;
	}

	return out;
}
