/*
 * Copyright (C) 2012-2017 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#include "httpheaders.h"

// return position, end of string if not found, -1 on error
static int findNonQuoted(const CowByteArray &in, char c, int offset = 0)
{
	bool inQuote = false;

	for(int n = offset; n < in.size(); ++n)
	{
		char i = in[n];

		if(inQuote)
		{
			if(i == '\\')
			{
				++n;

				// no character after the escape
				if(n >= in.size())
				{
					return -1;
				}
			}
			else if(i == '\"')
				inQuote = false;
		}
		else
		{
			if(i == '\"')
			{
				inQuote = true;
			}
			else if(i == c)
			{
				return n;
			}
		}
	}

	// unterminated quote
	if(inQuote)
	{
		return -1;
	}

	return in.size();
}

// search for one of many chars
static int findNext(const CowByteArray &in, const char *charList, int offset = 0)
{
	int len = qstrlen(charList);
	for(int n = offset; n < in.size(); ++n)
	{
		char c = in[n];
		for(int i = 0; i < len; ++i)
		{
			if(c == charList[i])
				return n;
		}
	}

	return -1;
}

static CowByteArrayList headerSplit(const CowByteArray &in)
{
	CowByteArrayList parts;
	int pos = 0;
	while(pos < in.size())
	{
		int end = findNonQuoted(in, ',', pos);
		if(end != -1)
		{
			parts += in.mid(pos, end - pos).trimmed();

			if(end < in.size())
				pos = end + 1;
			else
				pos = in.size();
		}
		else
		{
			parts += in.mid(pos).trimmed();

			pos = in.size();
		}
	}
	return parts;
}

bool HttpHeaderParameters::contains(const CowByteArray &key) const
{
	for(int n = 0; n < count(); ++n)
	{
		if(qstricmp(at(n).first.data(), key.data()) == 0)
			return true;
	}

	return false;
}

CowByteArray HttpHeaderParameters::get(const CowByteArray &key) const
{
	for(int n = 0; n < count(); ++n)
	{
		const HttpHeaderParameter &h = at(n);
		if(qstricmp(h.first.data(), key.data()) == 0)
			return h.second;
	}

	return CowByteArray();
}

bool HttpHeaders::contains(const CowByteArray &key) const
{
	for(int n = 0; n < count(); ++n)
	{
		if(qstricmp(at(n).first.data(), key.data()) == 0)
			return true;
	}

	return false;
}

CowByteArray HttpHeaders::get(const CowByteArray &key) const
{
	for(int n = 0; n < count(); ++n)
	{
		const HttpHeader &h = at(n);
		if(qstricmp(h.first.data(), key.data()) == 0)
			return h.second;
	}

	return CowByteArray();
}

HttpHeaderParameters HttpHeaders::getAsParameters(const CowByteArray &key, ParseMode mode) const
{
	CowByteArray h = get(key);
	if(h.isEmpty())
		return HttpHeaderParameters();

	return parseParameters(h, mode);
}

CowByteArray HttpHeaders::getAsFirstParameter(const CowByteArray &key) const
{
	HttpHeaderParameters p = getAsParameters(key);
	if(p.isEmpty())
		return CowByteArray();

	return p[0].first;
}

CowByteArrayList HttpHeaders::getAll(const CowByteArray &key, bool split) const
{
	CowByteArrayList out;

	for(int n = 0; n < count(); ++n)
	{
		const HttpHeader &h = at(n);
		if(qstricmp(h.first.data(), key.data()) == 0)
		{
			if(split)
				out += headerSplit(h.second);
			else
				out += h.second;
		}
	}

	return out;
}

QList<HttpHeaderParameters> HttpHeaders::getAllAsParameters(const CowByteArray &key, ParseMode mode, bool split) const
{
	QList<HttpHeaderParameters> out;

	CowByteArrayList l = getAll(key, split);
	for(CowByteArrayConstRef h : std::as_const(l))
	{
		bool ok;
		HttpHeaderParameters params = parseParameters(h, mode, &ok);
		if(ok)
			out += params;
	}

	return out;
}

CowByteArrayList HttpHeaders::takeAll(const CowByteArray &key, bool split)
{
	CowByteArrayList out;

	for(int n = 0; n < count(); ++n)
	{
		const HttpHeader &h = at(n);
		if(qstricmp(h.first.data(), key.data()) == 0)
		{
			if(split)
				out += headerSplit(h.second);
			else
				out += h.second;

			removeAt(n);
			--n; // adjust position
		}
	}

	return out;
}

void HttpHeaders::removeAll(const CowByteArray &key)
{
	for(int n = 0; n < count(); ++n)
	{
		if(qstricmp(at(n).first.data(), key.data()) == 0)
		{
			removeAt(n);
			--n; // adjust position
		}
	}
}

CowByteArray HttpHeaders::join(const CowByteArrayList &values)
{
	CowByteArray out;

	bool first = true;
	for(CowByteArrayConstRef val : std::as_const(values))
	{
		if(!first)
			out += ", ";

		out += val;
		first = false;
	}

	return out;
}

HttpHeaderParameters HttpHeaders::parseParameters(const CowByteArray &in, ParseMode mode, bool *ok)
{
	HttpHeaderParameters out;

	int start = 0;
	if(mode == NoParseFirstParameter)
	{
		int at = in.indexOf(';');
		if(at != -1)
		{
			out += HttpHeaderParameter(in.mid(0, at).trimmed(), CowByteArray());
			start = at + 1;
		}
		else
		{
			out += HttpHeaderParameter(in.trimmed(), CowByteArray());
			start = in.size();
		}
	}

	while(start < in.size())
	{
		CowByteArray var;
		CowByteArray val;

		int at = findNext(in, "=;", start);
		if(at != -1)
		{
			var = in.mid(start, at - start).trimmed();
			if(in[at] == '=')
			{
				++at;

				if(at < in.size() && in[at] == '\"')
				{
					++at;

					bool complete = false;
					for(int n = at; n < in.size(); ++n)
					{
						if(in[n] == '\\')
						{
							if(n + 1 >= in.size())
							{
								if(ok)
									*ok = false;
								return HttpHeaderParameters();
							}

							++n;
							val += in[n];
						}
						else if(in[n] == '\"')
						{
							complete = true;
							at = n + 1;
							break;
						}
						else
							val += in[n];
					}

					if(!complete)
					{
						if(ok)
							*ok = false;
						return HttpHeaderParameters();
					}

					at = in.indexOf(';', at);
					if(at != -1)
						start = at + 1;
					else
						start = in.size();
				}
				else
				{
					int vstart = at;
					at = in.indexOf(';', vstart);
					if(at != -1)
					{
						val = in.mid(vstart, at - vstart).trimmed();
						start = at + 1;
					}
					else
					{
						val = in.mid(vstart).trimmed();
						start = in.size();
					}
				}
			}
			else
				start = at + 1;
		}
		else
		{
			var = in.mid(start).trimmed();
			start = in.size();
		}

		out.append(HttpHeaderParameter(var, val));
	}

	if(ok)
		*ok = true;

	return out;
}
