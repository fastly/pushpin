/*
 * Copyright (C) 2015 Fanout, Inc.
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

#include "cors.h"

#include "httpheaders.h"

namespace Cors {

static bool isSimpleHeader(const QByteArray &in)
{
	return (qstricmp(in.data(), "Cache-Control") == 0 ||
		qstricmp(in.data(), "Content-Language") == 0 ||
		qstricmp(in.data(), "Content-Length") == 0 ||
		qstricmp(in.data(), "Content-Type") == 0 ||
		qstricmp(in.data(), "Expires") == 0 ||
		qstricmp(in.data(), "Last-Modified") == 0 ||
		qstricmp(in.data(), "Pragma") == 0);
}

static bool headerNamesContains(const QList<QByteArray> &names, const QByteArray &name)
{
	foreach(const QByteArray &i, names)
	{
		if(qstricmp(name.data(), i.data()) == 0)
			return true;
	}

	return false;
}

static bool headerNameStartsWith(const QByteArray &name, const char *value)
{
	return (qstrnicmp(name.data(), value, qstrlen(value)) == 0);
}

void applyCorsHeaders(const HttpHeaders &requestHeaders, HttpHeaders *responseHeaders)
{
	if(!responseHeaders->contains("Access-Control-Allow-Methods"))
	{
		QByteArray method = requestHeaders.get("Access-Control-Request-Method");

		if(!method.isEmpty())
			*responseHeaders += HttpHeader("Access-Control-Allow-Methods", method);
		else
			*responseHeaders += HttpHeader("Access-Control-Allow-Methods", "OPTIONS, HEAD, GET, POST, PUT, DELETE");
	}

	if(!responseHeaders->contains("Access-Control-Allow-Headers"))
	{
		QList<QByteArray> allowHeaders;
		foreach(const QByteArray &h, requestHeaders.getAll("Access-Control-Request-Headers", true))
		{
			if(!h.isEmpty())
				allowHeaders += h;
		}

		if(!allowHeaders.isEmpty())
			*responseHeaders += HttpHeader("Access-Control-Allow-Headers", HttpHeaders::join(allowHeaders));
	}

	if(!responseHeaders->contains("Access-Control-Expose-Headers"))
	{
		QList<QByteArray> exposeHeaders;
		foreach(const HttpHeader &h, *responseHeaders)
		{
			if(!isSimpleHeader(h.first) && !headerNameStartsWith(h.first, "Access-Control-") && !headerNameStartsWith(h.first, "Grip-") && !headerNamesContains(exposeHeaders, h.first))
				exposeHeaders += h.first;
		}

		if(!exposeHeaders.isEmpty())
			*responseHeaders += HttpHeader("Access-Control-Expose-Headers", HttpHeaders::join(exposeHeaders));
	}

	if(!responseHeaders->contains("Access-Control-Allow-Credentials"))
		*responseHeaders += HttpHeader("Access-Control-Allow-Credentials", "true");

	if(!responseHeaders->contains("Access-Control-Allow-Origin"))
	{
		QByteArray origin = requestHeaders.get("Origin");

		if(origin.isEmpty())
			origin = "*";

		*responseHeaders += HttpHeader("Access-Control-Allow-Origin", origin);
	}

	if(!responseHeaders->contains("Access-Control-Max-Age"))
		*responseHeaders += HttpHeader("Access-Control-Max-Age", "3600");
}

}
