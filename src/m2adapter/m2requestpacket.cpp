/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#include "m2requestpacket.h"

#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
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
	type((Type)-1),
	uploadDone(false),
	uploadStreamOffset(-1),
	uploadStreamDone(false),
	downloadCredits(-1),
	frameFlags(0)
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

	start = end + 1;
	TnetString::Type htype;
	int offset, size;
	if(!TnetString::check(in, start, &htype, &offset, &size))
		return false;

	if(htype != TnetString::Hash && htype != TnetString::ByteArray)
		return false;

	bool ok;
	QVariant vheaders = TnetString::toVariant(in, start, htype, offset, size, &ok);
	if(!ok)
		return false;

	QSet<QString> skipHeaders;
	skipHeaders += "x-mongrel2-upload-start";
	skipHeaders += "x-mongrel2-upload-done";

	headers.clear(); // will store full headers
	QMap<QString, QByteArray> m2headers; // single-value map for easy processing
	if(htype == TnetString::Hash)
	{
		QVariantMap headersMap = vheaders.toMap();
		QMapIterator<QString, QVariant> vit(headersMap);
		while(vit.hasNext())
		{
			vit.next();

			QString key = vit.key();
			QVariant val = vit.value();

			if(val.type() == QVariant::ByteArray)
			{
				QByteArray ba = val.toByteArray();

				m2headers[key] = ba;

				if(!isAllCaps(key) && !skipHeaders.contains(key))
					headers += HttpHeader(makeMixedCaseHeader(key).toLatin1(), ba);
			}
			else if(val.type() == QVariant::List)
			{
				QVariantList vl = val.toList();
				if(vl.isEmpty())
					return false;

				if(vl[0].type() != QVariant::ByteArray)
					return false;

				m2headers[key] = vl[0].toByteArray();

				if(!isAllCaps(key) && !skipHeaders.contains(key))
				{
					QByteArray name = makeMixedCaseHeader(key).toLatin1();

					foreach(const QVariant &v, vl)
					{
						if(v.type() != QVariant::ByteArray)
							return false;

						headers += HttpHeader(name, v.toByteArray());
					}
				}
			}
			else
				return false;
		}
	}
	else // ByteArray
	{
		QJsonParseError error;
		QJsonDocument doc = QJsonDocument::fromJson(vheaders.toByteArray(), &error);
		if(error.error != QJsonParseError::NoError || !doc.isObject())
			return false;

		QVariantMap headersMap = doc.object().toVariantMap();
		QMapIterator<QString, QVariant> vit(headersMap);
		while(vit.hasNext())
		{
			vit.next();

			QString key = vit.key();
			QVariant val = vit.value();

			if(val.type() == QVariant::String)
			{
				QByteArray ba = val.toString().toUtf8();

				m2headers[key] = ba;

				if(!isAllCaps(key) && !skipHeaders.contains(key))
					headers += HttpHeader(makeMixedCaseHeader(key).toLatin1(), ba);
			}
			else if(val.type() == QVariant::List)
			{
				QVariantList vl = val.toList();
				if(vl.isEmpty())
					return false;

				if(vl[0].type() != QVariant::String)
					return false;

				m2headers[key] = vl[0].toString().toUtf8();

				if(!isAllCaps(key) && !skipHeaders.contains(key))
				{
					QByteArray name = makeMixedCaseHeader(key).toLatin1();

					foreach(const QVariant &v, vl)
					{
						if(v.type() != QVariant::String)
							return false;

						headers += HttpHeader(name, v.toString().toUtf8());
					}
				}
			}
			else
				return false;
		}
	}

	start = offset + size + 1;
	TnetString::Type btype;
	if(!TnetString::check(in, start, &btype, &offset, &size))
		return false;

	if(btype != TnetString::ByteArray)
		return false;

	body = TnetString::toByteArray(in, start, offset, size, &ok);
	if(!ok)
		return false;

	scheme = m2headers.value("URL_SCHEME");
	version = m2headers.value("VERSION");

	QByteArray m2method = m2headers.value("METHOD");

	if(m2headers.contains("DOWNLOAD_CREDITS"))
		downloadCredits = m2headers.value("DOWNLOAD_CREDITS").toInt();

	if(m2method == "JSON")
	{
		QJsonParseError error;
		QJsonDocument doc = QJsonDocument::fromJson(body, &error);
		if(error.error != QJsonParseError::NoError || !doc.isObject())
			return false;

		QVariantMap data = doc.object().toVariantMap();
		if(!data.contains("type") || data["type"].type() != QVariant::String)
			return false;

		QString jtype = data["type"].toString();

		if(jtype == "disconnect")
			type = Disconnect;
		else if(jtype == "credits")
			type = Credits;
		else
			return false;

		return true;
	}

	QByteArray m2RemoteAddr = m2headers.value("REMOTE_ADDR");

	method = QString::fromLatin1(m2method);
	uri = m2headers.value("URI");

	remoteAddress = QHostAddress();
	if(!m2RemoteAddr.isEmpty())
		remoteAddress = QHostAddress(QString::fromLatin1(m2RemoteAddr));

	if(m2method == "WEBSOCKET_HANDSHAKE")
	{
		type = WebSocketHandshake;
		return true;
	}
	else if(m2method == "WEBSOCKET")
	{
		type = WebSocketFrame;

		QByteArray flagsStr = m2headers.value("FLAGS");
		frameFlags = flagsStr.toInt(&ok, 16);
		return ok;
	}

	type = HttpRequest;

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

	QByteArray uploadStreamRaw = m2headers.value("UPLOAD_STREAM");
	QByteArray uploadStreamDoneRaw = m2headers.value("UPLOAD_STREAM_DONE");
	if(!uploadStreamRaw.isEmpty())
		uploadStreamOffset = uploadStreamRaw.toInt();
	if(!uploadStreamDoneRaw.isEmpty() && uploadStreamDoneRaw != "0")
		uploadStreamDone = true;

	return true;
}
