/*
 * Copyright (C) 2017-2019 Fanout, Inc.
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

#include "idformat.h"

#include <ctype.h>
#include "format.h"

namespace IdFormat {

class IdFormatHandler : public Format::Handler
{
public:
	QHash<QString, QByteArray> vars;

	virtual QByteArray handle(char type, const QByteArray &arg, QString *error) const
	{
		if(type != 's')
		{
			*error = QString("Unknown directive '%1'").arg(type);
			return QByteArray();
		}

		if(arg.isNull())
		{
			*error = QString("Directive 's' requires argument");
			return QByteArray();
		}

		QByteArray value = vars.value(arg);
		if(value.isNull())
		{
			*error = QString("No such variable '%1'").arg(QString::fromUtf8(arg));
			return QByteArray();
		}

		return value;
	}
};

class ContentFormatHandler : public Format::Handler
{
public:
	QByteArray defaultId;
	bool hex;

	ContentFormatHandler() :
		hex(false)
	{
	}

	virtual QByteArray handle(char type, const QByteArray &arg, QString *error) const
	{
		if(type != 'I')
		{
			*error = QString("Unknown directive '%1'").arg(type);
			return QByteArray();
		}

		QByteArray id;
		if(!arg.isNull())
		{
			id = arg;
		}
		else
		{
			if(defaultId.isNull())
			{
				*error = QString("No ID specified and no default ID in context");
				return QByteArray();
			}

			id = defaultId;
		}

		if(hex)
		{
			id = id.toHex();
		}

		return id;
	}
};

ContentRenderer::ContentRenderer(const QByteArray &defaultId, bool hex) :
	defaultId_(defaultId),
	hex_(hex)
{
}

QByteArray ContentRenderer::update(const QByteArray &data)
{
	buf_ += data;

	ContentFormatHandler handler;
	handler.defaultId = defaultId_;
	handler.hex = hex_;

	int partialPos;

	QByteArray ret = Format::process(buf_, &handler, &partialPos, &errorMessage_);
	if(!ret.isNull())
	{
		buf_ = buf_.mid(partialPos);
	}

	return ret;
}

QByteArray ContentRenderer::finalize()
{
	QByteArray data = buf_;
	buf_.clear();

	ContentFormatHandler handler;
	handler.defaultId = defaultId_;
	handler.hex = hex_;
	return Format::process(data, &handler, 0, &errorMessage_);
}

QByteArray ContentRenderer::process(const QByteArray &data)
{
	ContentFormatHandler handler;
	handler.defaultId = defaultId_;
	handler.hex = hex_;
	return Format::process(data, &handler, 0, &errorMessage_);
}

QByteArray renderId(const QByteArray &data, const QHash<QString, QByteArray> &vars, QString *error)
{
	IdFormatHandler handler;
	handler.vars = vars;
	return Format::process(data, &handler, 0, error);
}

}
