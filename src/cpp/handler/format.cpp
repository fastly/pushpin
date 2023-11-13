/*
 * Copyright (C) 2019 Fanout, Inc.
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
