/*
 * Copyright (C) 2017-2019 Fanout, Inc.
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
