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

#include "filterstack.h"

FilterStack::FilterStack(const Filter::Context &context, const QStringList &filters)
{
	foreach(const QString &name, filters)
	{
		Filter *f = Filter::create(name);
		if(f)
		{
			f->setContext(context);
			filters_ += f;
		}
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
