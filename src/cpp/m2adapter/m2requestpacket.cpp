/*
 * Copyright (C) 2012-2013 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include "m2requestpacket.h"

#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include "qtcompat.h"
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

			if(typeId(val) == QMetaType::QByteArray)
			{
				QByteArray ba = val.toByteArray();

				m2headers[key] = ba;

				if(!isAllCaps(key) && !skipHeaders.contains(key))
					headers += HttpHeader(makeMixedCaseHeader(key).toLatin1(), ba);
			}
			else if(typeId(val) == QMetaType::QVariantList)
			{
				QVariantList vl = val.toList();
				if(vl.isEmpty())
					return false;

				if(typeId(vl[0]) != QMetaType::QByteArray)
					return false;

				m2headers[key] = vl[0].toByteArray();

				if(!isAllCaps(key) && !skipHeaders.contains(key))
				{
					QByteArray name = makeMixedCaseHeader(key).toLatin1();

					foreach(const QVariant &v, vl)
					{
						if(typeId(v) != QMetaType::QByteArray)
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

			if(typeId(val) == QMetaType::QString)
			{
				QByteArray ba = val.toString().toUtf8();

				m2headers[key] = ba;

				if(!isAllCaps(key) && !skipHeaders.contains(key))
					headers += HttpHeader(makeMixedCaseHeader(key).toLatin1(), ba);
			}
			else if(typeId(val) == QMetaType::QVariantList)
			{
				QVariantList vl = val.toList();
				if(vl.isEmpty())
					return false;

				if(typeId(vl[0]) != QMetaType::QString)
					return false;

				m2headers[key] = vl[0].toString().toUtf8();

				if(!isAllCaps(key) && !skipHeaders.contains(key))
				{
					QByteArray name = makeMixedCaseHeader(key).toLatin1();

					foreach(const QVariant &v, vl)
					{
						if(typeId(v) != QMetaType::QString)
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
		if(!data.contains("type") || typeId(data["type"]) != QMetaType::QString)
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
