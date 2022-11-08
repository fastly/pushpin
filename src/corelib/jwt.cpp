/*
 * Copyright (C) 2012-2016 Fanout, Inc.
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

#include "jwt.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
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
	return base64url(QMessageAuthenticationCode::hash(si, key, QCryptographicHash::Sha256));
}

namespace Jwt {

QByteArray encode(const QVariant &claim, const QByteArray &key)
{
	QVariantMap header;
	header["typ"] = "JWT";
	header["alg"] = "HS256";
	QByteArray headerJson = QJsonDocument(QJsonObject::fromVariantMap(header)).toJson(QJsonDocument::Compact);
	if(headerJson.isNull())
		return QByteArray();

	QByteArray claimJson = QJsonDocument(QJsonObject::fromVariantMap(claim.toMap())).toJson(QJsonDocument::Compact);
	if(claimJson.isNull())
		return QByteArray();

	QByteArray headerPart = base64url(headerJson);
	QByteArray claimPart = base64url(claimJson);

	QByteArray sig = jws_sign(headerPart, claimPart, key);

	return headerPart + '.' + claimPart + '.' + sig;
}

QVariant decode(const QByteArray &token, const QByteArray &key)
{
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

	QByteArray headerJson = unbase64url(headerPart);
	if(headerJson.isEmpty())
		return QVariant();

	QJsonParseError error;
	QJsonDocument doc = QJsonDocument::fromJson(headerJson, &error);
	if(error.error != QJsonParseError::NoError || !doc.isObject())
		return QVariant();

	QVariantMap header = doc.object().toVariantMap();
	if(header.value("typ").toString() != "JWT" || header.value("alg").toString() != "HS256")
		return QVariant();

	QByteArray claimJson = unbase64url(claimPart);
	if(claimJson.isEmpty())
		return QVariant();

	doc = QJsonDocument::fromJson(claimJson, &error);
	if(error.error != QJsonParseError::NoError || !doc.isObject())
		return QVariant();

	QVariant claim = doc.object().toVariantMap();

	if(jws_sign(headerPart, claimPart, key) != sig)
		return QVariant();

	return claim;
}

}
