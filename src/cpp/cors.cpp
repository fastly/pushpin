/*
 * Copyright (C) 2015 Fanout, Inc.
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
