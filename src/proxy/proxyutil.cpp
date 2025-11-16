/*
 * Copyright (C) 2014-2022 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#include "proxyutil.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include "qtcompat.h"
#include "log.h"
#include "jwt.h"
#include "inspectdata.h"

static QByteArray make_token(const QByteArray &iss, const Jwt::EncodingKey &key)
{
	QVariantMap claim;
	claim["iss"] = QString::fromUtf8(iss);
	claim["exp"] = QDateTime::currentDateTimeUtc().toSecsSinceEpoch() + 3600;
	return Jwt::encode(claim, key);
}

static bool validate_token(const QByteArray &token, const Jwt::DecodingKey &key)
{
	QVariant claimObj = Jwt::decode(token, key);
	if(!claimObj.isValid() || typeId(claimObj) != QMetaType::QVariantMap)
		return false;

	QVariantMap claim = claimObj.toMap();

	int exp = claim.value("exp").toInt();
	if(exp <= 0 || (int)QDateTime::currentDateTimeUtc().toSecsSinceEpoch() >= exp)
		return false;

	return true;
}

namespace ProxyUtil {

// Check if the request is coming from a grip proxy already
bool checkTrustedClient(const char *logprefix, void *object, const HttpRequestData &requestData, const Jwt::DecodingKey &defaultUpstreamKey)
{
	if(!defaultUpstreamKey.isNull())
	{
		QByteArray token = requestData.headers.get("Grip-Sig").asQByteArray();
		if(!token.isEmpty())
		{
			if(validate_token(token, defaultUpstreamKey))
				return true;

			log_debug("%s: %p signature present but invalid: %s", logprefix, object, token.data());
		}
	}

	return false;
}

void manipulateRequestHeaders(const char *logprefix, void *object, HttpRequestData *requestData, bool trustedClient, const DomainMap::Entry &entry, const QByteArray &sigIss, const Jwt::EncodingKey &sigKey, bool acceptXForwardedProtocol, bool useXForwardedProto, bool useXForwardedProtocol, const XffRule &xffTrustedRule, const XffRule &xffRule, const QList<QByteArray> &origHeadersNeedMark, bool acceptPushpinRoute, const QByteArray &cdnLoop, const QHostAddress &peerAddress, const InspectData &idata, bool gripEnabled, bool intReq)
{
	if(trustedClient)
		log_debug("%s: %p passing to upstream", logprefix, object);

	if(!trustedClient && entry.origHeaders)
	{
		// Copy headers to include magic prefix, so that the original
		// headers may be recovered later. If the client is trusted,
		// then we assume this has been done already.

		HttpHeaders origHeaders;
		for(int n = 0; n < requestData->headers.count(); ++n)
		{
			const HttpHeader &h = requestData->headers[n];

			if(qstrnicmp(h.first.data(), "eb9bf0f5-", 9) == 0)
			{
				// If it's already marked, take it
				origHeaders += h;

				// Remove where it lives now. We'll put it back later
				requestData->headers.removeAt(n);
				--n; // Adjust position
			}
			else
			{
				// See if we require it to be marked already
				bool found = false;
				foreach(const QByteArray &i, origHeadersNeedMark)
				{
					if(qstricmp(h.first.data(), i.data()) == 0)
					{
						found = true;
						break;
					}
				}

				// If not, then add as marked
				if(!found)
					origHeaders += HttpHeader("eb9bf0f5-" + h.first, h.second);
			}
		}

		// Now append all the orig headers to the end
		foreach(const HttpHeader &h, origHeaders)
			requestData->headers += h;
	}
	else if(!entry.origHeaders)
	{
		// If we don't want original headers, then filter them out
		// before proxying
		for(int n = 0; n < requestData->headers.count(); ++n)
		{
			const HttpHeader &h = requestData->headers[n];

			if(qstrnicmp(h.first.data(), "eb9bf0f5-", 9) == 0)
			{
				requestData->headers.removeAt(n);
				--n; // Adjust position
			}
		}
	}

	// Don't relay these headers. Their meaning is handled by
	// mongrel2 and they only apply to the incoming hop.
	requestData->headers.removeAll("Connection");
	requestData->headers.removeAll("Keep-Alive");
	requestData->headers.removeAll("Accept-Encoding");
	requestData->headers.removeAll("Content-Encoding");
	requestData->headers.removeAll("Transfer-Encoding");
	requestData->headers.removeAll("Expect");

	if(acceptPushpinRoute)
		requestData->headers.removeAll("Pushpin-Route");

	if(!cdnLoop.isEmpty())
	{
		QList<QByteArray> values = requestData->headers.takeAll("CDN-Loop", true).asQByteArrayList();
		values += cdnLoop;

		requestData->headers += HttpHeader("CDN-Loop", values.join(", "));
	}

	if(!trustedClient && !intReq)
	{
		// Remove all Grip- headers
		for(int n = 0; n < requestData->headers.count(); ++n)
		{
			if(qstrnicmp(requestData->headers[n].first.data(), "Grip-", 5) == 0)
			{
				requestData->headers.removeAt(n);
				--n; // Adjust position
			}
		}
	}

	if(!trustedClient && gripEnabled)
	{
		applyGripSig(logprefix, object, &requestData->headers, sigIss, sigKey);

		requestData->headers.removeAll("Grip-Feature");
		requestData->headers += HttpHeader("Grip-Feature",
			"status, session, link:next, link:gone, filter:skip-self, filter:skip-users, filter:require-sub, filter:build-id, filter:var-subst, filter:http-check, filter:http-modify");

		if(!idata.sid.isEmpty())
		{
			requestData->headers.removeAll("Grip-Session-Id");
			requestData->headers += HttpHeader("Grip-Session-Id", idata.sid);
		}

		if(!idata.lastIds.isEmpty())
		{
			QHashIterator<QByteArray, QByteArray> it(idata.lastIds);
			while(it.hasNext())
			{
				it.next();
				requestData->headers += HttpHeader("Grip-Last", it.key() + "; last-id=" + it.value());
			}
		}
	}

	if(acceptXForwardedProtocol || useXForwardedProto || useXForwardedProtocol)
	{
		requestData->headers.removeAll("X-Forwarded-Proto");

		// TODO: deprecate
		requestData->headers.removeAll("X-Forwarded-Protocol");
	}

	if(useXForwardedProto || useXForwardedProtocol)
	{
		QString scheme = requestData->uri.scheme();
		if(scheme == "https" || scheme == "wss")
		{
			QByteArray schemeVal = scheme.toUtf8();

			if(useXForwardedProto)
				requestData->headers += HttpHeader("X-Forwarded-Proto", schemeVal);

			// TODO: deprecate
			if(useXForwardedProtocol)
				requestData->headers += HttpHeader("X-Forwarded-Protocol", schemeVal);
		}
	}

	const XffRule *xr;
	if(trustedClient)
		xr = &xffTrustedRule;
	else
		xr = &xffRule;

	QList<QByteArray> xffValues = requestData->headers.takeAll("X-Forwarded-For").asQByteArrayList();
	if(xr->truncate >= 0)
		xffValues = xffValues.mid(qMax(xffValues.count() - xr->truncate, 0));
	if(xr->append)
		xffValues += peerAddress.toString().toUtf8();
	if(!xffValues.isEmpty())
		requestData->headers += HttpHeader("X-Forwarded-For", HttpHeaders::join(xffValues));
}

void applyHost(QUrl *url, const QString &host)
{
	int at = host.indexOf(':');
	if(at != -1)
	{
		url->setHost(host.mid(0, at));
		url->setPort(host.mid(at + 1).toInt());
	}
	else
	{
		url->setHost(host);
		url->setPort(-1);
	}
}

void applyHostHeader(HttpHeaders *headers, const QUrl &uri)
{
	QByteArray hostHeader = uri.host().toUtf8();
	if(uri.port() != -1)
		hostHeader += ':' + QByteArray::number(uri.port());

	if(headers->get("Host").asQByteArray() != hostHeader)
	{
		headers->removeAll("Host");
		headers->append(HttpHeader("Host", hostHeader));
	}
}

void applyGripSig(const char *logprefix, void *object, HttpHeaders *headers, const QByteArray &sigIss, const Jwt::EncodingKey &sigKey)
{
	if(!sigIss.isEmpty() && !sigKey.isNull())
	{
		QByteArray token = make_token(sigIss, sigKey);
		if(!token.isEmpty())
		{
			headers->removeAll("Grip-Sig");
			headers->append(HttpHeader("Grip-Sig", token));
		}
		else
			log_error("%s: %p failed to sign request", logprefix, object);
	}
}

QString targetToString(const DomainMap::Target &target)
{
	if(target.type == DomainMap::Target::Test)
	{
		return "test";
	}
	else if(target.type == DomainMap::Target::Custom)
	{
		return(target.zhttpRoute.req ? "zhttpreq/" : "zhttp/") + target.zhttpRoute.baseSpec;
	}
	else // Default
	{
		QString host;
		if(QHostAddress(target.connectHost).protocol() == QAbstractSocket::IPv6Protocol)
			host = '[' + target.connectHost + ']';
		else
			host = target.connectHost;

		return host + ':' + QString::number(target.connectPort);
	}
}

QHostAddress getLogicalAddress(const HttpHeaders &headers, const XffRule &xffRule, const QHostAddress &peerAddress)
{
	QList<QByteArray> xffValues = headers.getAll("X-Forwarded-For").asQByteArrayList();
	if(!xffValues.isEmpty() && xffRule.truncate != 0)
	{
		QHostAddress addr;
		if(addr.setAddress(QString::fromUtf8(xffValues.first())))
			return addr;
	}

	return peerAddress;
}

}
