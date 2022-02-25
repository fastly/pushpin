/*
 * Copyright (C) 2014-2019 Fanout, Inc.
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

#include "proxyutil.h"

#include <QDateTime>
#include "log.h"
#include "jwt.h"
#include "inspectdata.h"

static QByteArray make_token(const QByteArray &iss, const QByteArray &key)
{
	QVariantMap claim;
	claim["iss"] = QString::fromUtf8(iss);
	claim["exp"] = QDateTime::currentDateTimeUtc().toTime_t() + 3600;
	return Jwt::encode(claim, key);
}

static bool validate_token(const QByteArray &token, const QByteArray &key)
{
	QVariant claimObj = Jwt::decode(token, key);
	if(!claimObj.isValid() || claimObj.type() != QVariant::Map)
		return false;

	QVariantMap claim = claimObj.toMap();

	int exp = claim.value("exp").toInt();
	if(exp <= 0 || (int)QDateTime::currentDateTimeUtc().toTime_t() >= exp)
		return false;

	return true;
}

namespace ProxyUtil {

// check if the request is coming from a grip proxy already
bool checkTrustedClient(const char *logprefix, void *object, const HttpRequestData &requestData, const QByteArray &defaultUpstreamKey)
{
	if(!defaultUpstreamKey.isEmpty())
	{
		QByteArray token = requestData.headers.get("Grip-Sig");
		if(!token.isEmpty())
		{
			if(validate_token(token, defaultUpstreamKey))
				return true;

			log_debug("%s: %p signature present but invalid: %s", logprefix, object, token.data());
		}
	}

	return false;
}

void manipulateRequestHeaders(const char *logprefix, void *object, HttpRequestData *requestData, bool trustedClient, const DomainMap::Entry &entry, const QByteArray &sigIss, const QByteArray &sigKey, bool acceptXForwardedProtocol, bool useXForwardedProto, bool useXForwardedProtocol, const XffRule &xffTrustedRule, const XffRule &xffRule, const QList<QByteArray> &origHeadersNeedMark, const QHostAddress &peerAddress, const InspectData &idata, bool gripEnabled, bool intReq)
{
	if(trustedClient)
		log_debug("%s: %p passing to upstream", logprefix, object);

	if(!trustedClient && entry.origHeaders)
	{
		// copy headers to include magic prefix, so that the original
		//   headers may be recovered later. if the client is trusted,
		//   then we assume this has been done already.

		HttpHeaders origHeaders;
		for(int n = 0; n < requestData->headers.count(); ++n)
		{
			const HttpHeader &h = requestData->headers[n];

			if(qstrnicmp(h.first.data(), "eb9bf0f5-", 9) == 0)
			{
				// if it's already marked, take it
				origHeaders += h;

				// remove where it lives now. we'll put it back later
				requestData->headers.removeAt(n);
				--n; // adjust position
			}
			else
			{
				// see if we require it to be marked already
				bool found = false;
				foreach(const QByteArray &i, origHeadersNeedMark)
				{
					if(qstricmp(h.first.data(), i.data()) == 0)
					{
						found = true;
						break;
					}
				}

				// if not, then add as marked
				if(!found)
					origHeaders += HttpHeader("eb9bf0f5-" + h.first, h.second);
			}
		}

		// now append all the orig headers to the end
		foreach(const HttpHeader &h, origHeaders)
			requestData->headers += h;
	}
	else if(!entry.origHeaders)
	{
		// if we don't want original headers, then filter them out
		//   before proxying
		for(int n = 0; n < requestData->headers.count(); ++n)
		{
			const HttpHeader &h = requestData->headers[n];

			if(qstrnicmp(h.first.data(), "eb9bf0f5-", 9) == 0)
			{
				requestData->headers.removeAt(n);
				--n; // adjust position
			}
		}
	}

	// don't relay these headers. their meaning is handled by
	//   mongrel2 and they only apply to the incoming hop.
	requestData->headers.removeAll("Connection");
	requestData->headers.removeAll("Keep-Alive");
	requestData->headers.removeAll("Accept-Encoding");
	requestData->headers.removeAll("Content-Encoding");
	requestData->headers.removeAll("Transfer-Encoding");
	requestData->headers.removeAll("Expect");

	if(!trustedClient && !intReq)
	{
		// remove all Grip- headers
		for(int n = 0; n < requestData->headers.count(); ++n)
		{
			if(qstrnicmp(requestData->headers[n].first.data(), "Grip-", 5) == 0)
			{
				requestData->headers.removeAt(n);
				--n; // adjust position
			}
		}
	}

	if(!trustedClient && gripEnabled)
	{
		// set Grip-Sig
		if(!sigIss.isEmpty() && !sigKey.isEmpty())
		{
			QByteArray token = make_token(sigIss, sigKey);
			if(!token.isEmpty())
			{
				requestData->headers.removeAll("Grip-Sig");
				requestData->headers += HttpHeader("Grip-Sig", token);
			}
			else
				log_error("%s: %p failed to sign request", logprefix, object);
		}

		requestData->headers.removeAll("Grip-Feature");
		requestData->headers += HttpHeader("Grip-Feature",
			"status, session, link:next, filter:skip-self, filter:skip-users, filter:require-sub, filter:build-id, filter:var-subst");

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

	QList<QByteArray> xffValues = requestData->headers.takeAll("X-Forwarded-For");
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

	if(headers->get("Host") != hostHeader)
	{
		headers->removeAll("Host");
		headers->append(HttpHeader("Host", hostHeader));
	}
}

QString targetToString(const DomainMap::Target &target)
{
	if(target.type == DomainMap::Target::Test)
		return "test";
	else if(target.type == DomainMap::Target::Custom)
		return(target.zhttpRoute.req ? "zhttpreq/" : "zhttp/") + target.zhttpRoute.baseSpec;
	else // Default
		return target.connectHost + ':' + QString::number(target.connectPort);
}

QHostAddress getLogicalAddress(const HttpHeaders &headers, const XffRule &xffRule, const QHostAddress &peerAddress)
{
	QList<QByteArray> xffValues = headers.getAll("X-Forwarded-For");
	if(!xffValues.isEmpty() && xffRule.truncate != 0)
	{
		QHostAddress addr;
		if(addr.setAddress(QString::fromUtf8(xffValues.first())))
			return addr;
	}

	return peerAddress;
}

}
