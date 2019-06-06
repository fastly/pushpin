/*
 * Copyright (C) 2019 Fanout, Inc.
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

#include "format.h"

#include <ctype.h>

namespace Format {

QByteArray process(const QByteArray &format, Handler *handler, int *partialPos, QString *error)
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

}
