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

#include "idformat.h"

#include <ctype.h>

namespace IdFormat {

class FormatHandler
{
public:
	virtual ~FormatHandler() {}

	// returns null array on error
	virtual QByteArray handle(char type, const QByteArray &arg, QString *error) const = 0;
};

static QByteArray processFormat(const QByteArray &format, FormatHandler *handler, int *partialPos = 0, QString *error = 0)
{
	QByteArray out("");
	for(int n = 0; n < format.length(); ++n)
	{
		char c = format.at(n);

		if(c == '%')
		{
			int markerPos = n;

			if(n + 1 >= format.length())
			{
				if(partialPos)
				{
					*partialPos = markerPos;
					return out;
				}
				else
				{
					if(error)
						*error = QString("Expected directive after '%' at position %1").arg(n);
					return QByteArray();
				}
			}

			++n;
			c = format.at(n);

			if(c == '(')
			{
				int fieldStart = n;

				if(n + 1 >= format.length())
				{
					if(partialPos)
					{
						*partialPos = markerPos;
						return out;
					}
					else
					{
						if(error)
							*error = QString("Expected character after '(' at position %1").arg(n);
						return QByteArray();
					}
				}

				++n;

				QByteArray arg;

				// scan for ')'
				bool ok = false;
				for(; n < format.length(); ++n)
				{
					c = format.at(n);

					if(c == '\\')
					{
						if(n + 1 >= format.length())
						{
							if(partialPos)
							{
								*partialPos = markerPos;
								return out;
							}
							else
							{
								if(error)
									*error = QString("Expected character after '\\' at position %1").arg(n);
								return QByteArray();
							}
						}

						++n;
						c = format.at(n);

						arg += c;
					}
					else if(c == ')')
					{
						ok = true;
						break;
					}
					else
					{
						arg += c;
					}
				}
				if(!ok)
				{
					if(partialPos)
					{
						*partialPos = markerPos;
						return out;
					}
					else
					{
						if(error)
							*error = QString("Unterminated field starting at position %1").arg(fieldStart);
						return QByteArray();
					}
				}

				if(n + 1 >= format.length())
				{
					if(partialPos)
					{
						*partialPos = markerPos;
						return out;
					}
					else
					{
						if(error)
							*error = QString("Expected directive after ')' at position %1").arg(n);
						return QByteArray();
					}
				}

				++n;
				c = format.at(n);

				QString _error;
				QByteArray result = handler->handle(c, arg, &_error);
				if(result.isNull())
				{
					if(error)
						*error = QString("%1 at position %2").arg(_error, QString::number(n));
					return QByteArray();
				}

				out += result;
			}
			else if(c == '%')
			{
				out += c;
			}
			else if(isalpha(c))
			{
				QString _error;
				QByteArray result = handler->handle(c, QByteArray(), &_error);
				if(result.isNull())
				{
					if(error)
						*error = QString("%1 at position %2").arg(_error, QString::number(n));
					return QByteArray();
				}

				out += result;
			}
			else
			{
				if(error)
					*error = QString("Unknown directive '%1' at position %2").arg(QString(c), QString::number(n));
				return QByteArray();
			}
		}
		else
		{
			out += c;
		}
	}

	if(partialPos)
		*partialPos = format.length();

	return out;
}

class IdFormatHandler : public FormatHandler
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

class ContentFormatHandler : public FormatHandler
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

	QByteArray ret = processFormat(buf_, &handler, &partialPos, &errorMessage_);
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
	return processFormat(data, &handler, 0, &errorMessage_);
}

QByteArray ContentRenderer::process(const QByteArray &data)
{
	ContentFormatHandler handler;
	handler.defaultId = defaultId_;
	handler.hex = hex_;
	return processFormat(data, &handler, 0, &errorMessage_);
}

QByteArray renderId(const QByteArray &data, const QHash<QString, QByteArray> &vars, QString *error)
{
	IdFormatHandler handler;
	handler.vars = vars;
	return processFormat(data, &handler, 0, error);
}

}
