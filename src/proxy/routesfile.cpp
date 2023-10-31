/*
 * Copyright (C) 2016-2022 Fanout, Inc.
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
							{
								Token token(Token::Error);
								token.value = "unterminated escape sequence";
								return token;
							}

							if(str_[at] == '\\')
								part += '\\';
							else if(str_[at] == '\"')
								part += '\"';
							else
							{
								Token token(Token::Error);
								token.value = QString("unexpected escape character: ") + str_[at];
								return token;
							}
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
					{
						Token token(Token::Error);
						token.value = "unterminated quoted section";
						return token;
					}

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
