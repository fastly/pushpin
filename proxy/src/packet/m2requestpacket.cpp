/*
 * Copyright (C) 2012-2013 Fan Out Networks, Inc.
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

#include "m2requestpacket.h"

#include <QSet>
#include <qjson/parser.h>
#include "tnetstring.h"

static bool isAllCaps(const QString &s)
{
	for(int n = 0; n < s.length(); ++n)
	{
		QChar c = s[n];

		// non-letters are allowed, so what we really check against is
		//   lowercase
		if(c.isLower())
			return false;
	}

	return true;
}

static QString makeMixedCaseHeader(const QString &s)
{
	QString out;
	for(int n = 0; n < s.length(); ++n)
	{
		QChar c = s[n];
		if(n == 0 || (n - 1 >= 0 && s[n - 1] == '-'))
			out += c.toUpper();
		else
			out += c;
	}

	return out;
}

M2RequestPacket::M2RequestPacket() :
	isDisconnect(false),
	uploadDone(false)
{
}

bool M2RequestPacket::fromByteArray(const QByteArray &in)
{
	int start = 0;
	int end = in.indexOf(' ');
	if(end == -1)
		return false;

	sender = in.mid(start, end - start);

	start = end + 1;
	end = in.indexOf(' ', start);
	if(end == -1)
		return false;

	id = in.mid(start, end - start);

	start = end + 1;
	end = in.indexOf(' ', start);
	if(end == -1)
		return false;

	QByteArray path_only = in.mid(start, end - start);

	start = end + 1;
	TnetString::Type type;
	int offset, size;
	if(!TnetString::check(in, start, &type, &offset, &size))
		return false;

	if(type != TnetString::Hash && type != TnetString::ByteArray)
		return false;

	bool ok;
	QVariant vheaders = TnetString::toVariant(in, start, type, offset, size, &ok);
	if(!ok)
		return false;

	QVariantMap headersMap;
	if(type == TnetString::Hash)
	{
		headersMap = vheaders.toMap();
	}
	else // ByteArray
	{
		QJson::Parser parser;
		vheaders = parser.parse(vheaders.toByteArray(), &ok);
		if(!ok)
			return false;

		headersMap = vheaders.toMap();
	}

	QMap<QString, QByteArray> m2headers;
	QMapIterator<QString, QVariant> vit(headersMap);
	while(vit.hasNext())
	{
		vit.next();

		if(vit.value().type() != QVariant::String)
			return false;

		m2headers[vit.key()] = vit.value().toString().toUtf8();
	}

	start = offset + size + 1;
	if(!TnetString::check(in, start, &type, &offset, &size))
		return false;

	if(type != TnetString::ByteArray)
		return false;

	body = TnetString::toByteArray(in, start, offset, size, &ok);
	if(!ok)
		return false;

	scheme = m2headers.value("URL_SCHEME");

	QByteArray m2method = m2headers.value("METHOD");

	if(m2method == "JSON")
	{
		QJson::Parser parser;
		QVariant vdata = parser.parse(body, &ok);
		if(!ok)
			return false;

		if(vdata.type() != QVariant::Map)
			return false;

		QVariantMap data = vdata.toMap();
		if(!data.contains("type") || data["type"].type() != QVariant::String)
			return false;

		QString type = data["type"].toString();
		if(type != "disconnect")
			return false;

		isDisconnect = true;
		return true;
	}

	method = QString::fromLatin1(m2method);
	path = m2headers.value("URI");

	QByteArray uploadStartRaw = m2headers.value("x-mongrel2-upload-start");
	QByteArray uploadDoneRaw = m2headers.value("x-mongrel2-upload-done");
	if(!uploadDoneRaw.isEmpty())
	{
		// these headers must match for the packet to be valid. not
		//   sure why mongrel2 can't enforce this for us but whatever
		if(uploadStartRaw != uploadDoneRaw)
			return false;

		uploadFile = QString::fromUtf8(uploadDoneRaw);
		uploadDone = true;
	}
	else if(!uploadStartRaw.isEmpty())
	{
		uploadFile = QString::fromUtf8(uploadStartRaw);
	}

	QSet<QString> skipHeaders;
	skipHeaders += "x-mongrel2-upload-start";
	skipHeaders += "x-mongrel2-upload-done";

	headers.clear();
	QMapIterator<QString, QByteArray> it(m2headers);
	while(it.hasNext())
	{
		it.next();

		QString key = it.key();
		if(isAllCaps(key) || skipHeaders.contains(key))
			continue;

		headers += HttpHeader(makeMixedCaseHeader(key).toLatin1(), it.value());
	}

	return true;
}
