/*
 * Copyright (C) 2014 Fanout, Inc.
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

bool manipulateRequestHeaders(const char *logprefix, void *object, HttpRequestData *requestData, const QByteArray &defaultUpstreamKey, const DomainMap::Entry &entry, const QByteArray &sigIss, const QByteArray &sigKey, bool acceptXForwardedProtocol, bool useXForwardedProtocol, const XffRule &xffTrustedRule, const XffRule &xffRule, const QList<QByteArray> &origHeadersNeedMark, const QHostAddress &peerAddress, const InspectData &idata)
{
	// check if the request is coming from a grip proxy already
	bool trustedClient = false;
	if(!defaultUpstreamKey.isEmpty())
	{
		QByteArray token = requestData->headers.get("Grip-Sig");
		if(!token.isEmpty())
		{
			if(validate_token(token, defaultUpstreamKey))
			{
				log_debug("%s: %p passing to upstream", logprefix, object);
				trustedClient = true;
			}
			else
				log_debug("%s: %p signature present but invalid: %s", logprefix, object, token.data());
		}
	}

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

	// rewrite the Host header to match the hostname of the destination URL.
	//   in practice, the only time the value should ever be different is
	//   if the original Host header had a port specified
	requestData->headers.removeAll("Host");
	requestData->headers += HttpHeader("Host", requestData->uri.host().toUtf8());

	if(!trustedClient)
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

		// set Grip-Sig
		if(!sigIss.isEmpty() && !sigKey.isEmpty())
		{
			QByteArray token = make_token(sigIss, sigKey);
			if(!token.isEmpty())
				requestData->headers += HttpHeader("Grip-Sig", token);
			else
				log_error("%s: %p failed to sign request", logprefix, object);
		}
	}

	requestData->headers.removeAll("Grip-Feature");
	requestData->headers += HttpHeader("Grip-Feature", "status");

	if(!idata.sid.isEmpty())
	{
		requestData->headers += HttpHeader("Grip-Session-Id", idata.sid);
		QHashIterator<QByteArray, QByteArray> it(idata.lastIds);
		while(it.hasNext())
		{
			it.next();
			requestData->headers += HttpHeader("Grip-Last", it.key() + "; last-id=" + it.value());
		}
	}

	if(acceptXForwardedProtocol || useXForwardedProtocol)
		requestData->headers.removeAll("X-Forwarded-Protocol");

	if(useXForwardedProtocol)
	{
		QString scheme = requestData->uri.scheme();
		if(scheme == "https" || scheme == "wss")
			requestData->headers += HttpHeader("X-Forwarded-Protocol", scheme.toUtf8());
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

	return trustedClient;
}

}
