/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#include "jwt.h"

#include <QtCrypto>
#include <qjson/serializer.h>
#include <qjson/parser.h>
#include "log.h"

static QByteArray base64url(const QByteArray &in)
{
	QByteArray b64 = in.toBase64();

	// trim trailing padding
	int at = b64.indexOf('=');
	if(at != -1)
		b64 = b64.mid(0, at);

	// character substitution
	for(int n = 0; n < b64.size(); ++n)
	{
		if(b64[n] == '+')
			b64[n] = '-';
		else if(b64[n] == '/')
			b64[n] = '_';
	}

	return b64;
}

static QByteArray unbase64url(const QByteArray &in)
{
	QByteArray b64 = in;

	// add padding
	int need = b64.size() % 4;
	for(int n = 0; n < need; ++n)
		b64 += '=';

	// character substitution
	for(int n = 0; n < b64.size(); ++n)
	{
		if(b64[n] == '-')
			b64[n] = '+';
		else if(b64[n] == '_')
			b64[n] = '/';
	}

	return QByteArray::fromBase64(b64);
}

static QByteArray jws_sign(const QByteArray &header, const QByteArray &claim, const QByteArray &key)
{
	QByteArray si = header + '.' + claim;
	return base64url(QCA::MessageAuthenticationCode("hmac(sha256)", key).process(si).toByteArray());
}

namespace Jwt {

QByteArray encode(const QVariant &claim, const QByteArray &key)
{
	if(!QCA::isSupported("hmac(sha256)"))
		return QByteArray();

	QJson::Serializer serializer;

	QVariantMap header;
	header["typ"] = "JWT";
	header["alg"] = "HS256";
	QByteArray headerJson = serializer.serialize(header);
	if(headerJson.isNull())
		return QByteArray();

	QByteArray claimJson = serializer.serialize(claim);
	if(claimJson.isNull())
		return QByteArray();

	QByteArray headerPart = base64url(headerJson);
	QByteArray claimPart = base64url(claimJson);

	QByteArray sig = jws_sign(headerPart, claimPart, key);

	return headerPart + '.' + claimPart + '.' + sig;
}

QVariant decode(const QByteArray &token, const QByteArray &key)
{
	if(!QCA::isSupported("hmac(sha256)"))
		return QVariant();

	int at = token.indexOf('.');
	if(at == -1)
		return QVariant();

	QByteArray headerPart = token.mid(0, at);

	++at;
	int start = at;
	at = token.indexOf('.', start);
	if(at == -1)
		return QVariant();

	QByteArray claimPart = token.mid(start, at - start);
	QByteArray sig = token.mid(at + 1);

	bool ok;
	QJson::Parser parser;

	QByteArray headerJson = unbase64url(headerPart);
	if(headerJson.isEmpty())
		return QVariant();

	QVariant headerObj = parser.parse(headerJson, &ok);
	if(!ok || headerObj.type() != QVariant::Map)
		return QVariant();

	QVariantMap header = headerObj.toMap();
	if(header.value("typ").toString() != "JWT" || header.value("alg").toString() != "HS256")
		return QVariant();

	QByteArray claimJson = unbase64url(claimPart);
	if(claimJson.isEmpty())
		return QVariant();

	QVariant claim = parser.parse(claimJson, &ok);
	if(!ok)
		return QVariant();

	if(jws_sign(headerPart, claimPart, key) != sig)
		return QVariant();

	return claim;
}

}
