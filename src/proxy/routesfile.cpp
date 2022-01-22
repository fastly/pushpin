/*
 * Copyright (C) 2016-2022 Fanout, Inc.
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

#include "routesfile.h"

#include <assert.h>
#include "log.h"

namespace RoutesFile {

static int findNext(const QString &str, const QString &chars, int offset = 0)
{
	for(int n = offset; n < str.length(); ++n)
	{
		if(chars.contains(str[n]))
			return n;
	}

	return -1;
}

class LineParser
{
public:
	class Token
	{
	public:
		enum Type
		{
			Error,
			InitialValue,
			Prop,
			EndOfLine
		};

		Type type;
		QString value; // error, initialvalue, prop
		QString name; // prop;

		Token() :
			type((Type)-1)
		{
		}

		Token(Type _type) :
			type(_type)
		{
		}
	};

	enum State
	{
		ReadSeparator,
		ReadInitialValue,
		ReadProp
	};

	State state_;
	QString str_;
	int index_;

	LineParser(const QString &line) :
		state_(ReadSeparator),
		str_(line),
		index_(0)
	{
	}

	Token nextToken()
	{
		int start = index_;
		QString part;

		while(true)
		{
			//log_debug("%d [%s] %d", (int)state_, qPrintable(str_), index_);

			if(state_ == ReadSeparator)
			{
				for(; index_ < str_.length(); ++index_)
				{
					if(str_[index_] != ' ')
						break;
				}

				if(index_ >= str_.length() || str_[index_] == '#')
					return Token(Token::EndOfLine);

				state_ = ReadInitialValue;
				continue;
			}
			else // ReadInitialValue, ReadProp
			{
				int at = findNext(str_, "\", #", index_);

				// quoted section?
				if(at != -1 && str_[at] == '\"')
				{
					part += str_.mid(index_, at - index_);
					++at;

					// decode inner string
					for(; at < str_.length(); ++at)
					{
						if(str_[at] == '\\')
						{
							++at;
							if(at >= str_.length())
								return Token(Token::Error);

							if(str_[at] == '\\')
								part += '\\';
							else if(str_[at] == '\"')
								part += '\"';
							else
								return Token(Token::Error);
						}
						else if(str_[at] == '\"')
						{
							break;
						}
						else
						{
							part += str_[at];
						}
					}
					if(at >= str_.length())
						return Token(Token::Error);

					index_ = at + 1;
					continue;
				}

				// all other chars, or end of string, means end of initial value or prop

				if(at == -1)
					at = str_.length();

				part += str_.mid(index_, at - index_);

				Token token;

				if(state_ == ReadInitialValue)
				{
					int n = part.indexOf('=');

					// '=' in initial value?
					if(n != -1)
					{
						// return empty initial value, re-read as a prop
						index_ = start;
						state_ = ReadProp;
						return Token(Token::InitialValue);
					}

					token.type = Token::InitialValue;
					token.value = part;
				}
				else // ReadProp
				{
					if(part.isEmpty())
					{
						Token token(Token::Error);
						token.value = "expecting prop";
						return token;
					}

					QString name;
					QString value;

					int n = part.indexOf('=');
					if(n != -1)
					{
						name = part.mid(0, n);
						value = part.mid(n + 1);
					}
					else
						name = part;

					if(name.isEmpty())
					{
						token.type = Token::Error;
						token.value = "empty prop name";
						return token;
					}

					token.type = Token::Prop;
					token.name = name;
					token.value = value;
				}

				if(at < str_.length() && str_[at] == ',')
				{
					++at;
					state_ = ReadProp;
				}
				else // space, #, or end of line
				{
					state_ = ReadSeparator;
				}

				index_ = at;

				return token;
			}
		}
	}
};

QList<RouteSection> parseLine(const QString &line, bool *ok, QString *errorMessage)
{
	QList<RouteSection> out;

	LineParser parser(line);
	bool done = false;
	while(!done)
	{
		LineParser::Token t = parser.nextToken();
		//log_debug("token: %d [%s]", (int)t.type, qPrintable(t.value));
		switch(t.type)
		{
			case LineParser::Token::Error:
				if(ok)
					*ok = false;
				if(errorMessage)
					*errorMessage = t.value;
				return QList<RouteSection>();
			case LineParser::Token::InitialValue:
				{
					RouteSection s;
					s.value = t.value;
					out += s;
				}
				break;
			case LineParser::Token::Prop:
				assert(!out.isEmpty());
				out.last().props.insert(t.name, t.value);
				break;
			case LineParser::Token::EndOfLine:
				done = true;
				break;
		}
	}

	if(ok)
		*ok = true;
	return out;
}

}
