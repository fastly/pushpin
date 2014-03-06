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

#include "wsproxysession.h"

#include <assert.h>
#include <QUrl>
#include <QHostAddress>
#include "packet/httprequestdata.h"
#include "log.h"
#include "zhttpmanager.h"
#include "zwebsocket.h"
#include "domainmap.h"
#include "xffrule.h"
#include "proxyutil.h"

#define PENDING_FRAMES_MAX 100

class WsProxySession::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Idle,
		Connecting,
		Connected,
		Closing
	};

	WsProxySession *q;
	State state;
	ZhttpManager *zhttpManager;
	DomainMap *domainMap;
	QByteArray defaultSigIss;
	QByteArray defaultSigKey;
	QByteArray defaultUpstreamKey;
	bool passToUpstream;
	bool useXForwardedProtocol;
	XffRule xffRule;
	XffRule xffTrustedRule;
	QList<QByteArray> origHeadersNeedMark;
	HttpRequestData requestData;
	ZWebSocket *inSock;
	ZWebSocket *outSock;
	int inPending;
	int outPending;
	QByteArray channelPrefix;
	QList<DomainMap::Target> targets;

	Private(WsProxySession *_q, ZhttpManager *_zhttpManager, DomainMap *_domainMap) :
		QObject(_q),
		q(_q),
		state(Idle),
		zhttpManager(_zhttpManager),
		domainMap(_domainMap),
		passToUpstream(false),
		useXForwardedProtocol(false),
		inSock(0),
		outSock(0),
		inPending(0),
		outPending(0)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		delete inSock;
		inSock = 0;

		delete outSock;
		outSock = 0;
	}

	void start(ZWebSocket *sock)
	{
		assert(!inSock);

		state = Connecting;

		inSock = sock;
		inSock->setParent(this);
		connect(inSock, SIGNAL(readyRead()), SLOT(in_readyRead()));
		connect(inSock, SIGNAL(framesWritten(int)), SLOT(in_framesWritten(int)));
		connect(inSock, SIGNAL(peerClosed()), SLOT(in_peerClosed()));
		connect(inSock, SIGNAL(closed()), SLOT(in_closed()));
		connect(inSock, SIGNAL(error()), SLOT(in_error()));

		requestData.uri = inSock->requestUri();
		requestData.headers = inSock->requestHeaders();

		QString host = requestData.uri.host();
		bool isSecure = (requestData.uri.scheme() == "wss");

		DomainMap::Entry entry = domainMap->entry(DomainMap::WebSocket, isSecure, host, requestData.uri.encodedPath());
		if(entry.isNull())
		{
			log_warning("wsproxysession: %p %s has 0 routes", q, qPrintable(host));
			reject(502, "Bad Gateway", QString("No route for host: %1").arg(host));
			return;
		}

		QByteArray sigIss;
		QByteArray sigKey;
		if(!entry.sigIss.isEmpty() && !entry.sigKey.isEmpty())
		{
			sigIss = entry.sigIss;
			sigKey = entry.sigKey;
		}
		else
		{
			sigIss = defaultSigIss;
			sigKey = defaultSigKey;
		}

		channelPrefix = entry.prefix;
		targets = entry.targets;

		log_debug("wsproxysession: %p %s has %d routes", q, qPrintable(host), targets.count());

		bool trustedClient = ProxyUtil::manipulateRequestHeaders("wsproxysession", q, &requestData, defaultUpstreamKey, entry, sigIss, sigKey, useXForwardedProtocol, xffTrustedRule, xffRule, origHeadersNeedMark, inSock->peerAddress());

		if(trustedClient)
			passToUpstream = true;

		tryNextTarget();
	}

	void tryNextTarget()
	{
		if(targets.isEmpty())
		{
			reject(502, "Bad Gateway", "Error while proxying to origin.");
			return;
		}

		DomainMap::Target target = targets.takeFirst();

		QUrl uri = requestData.uri;
		if(target.ssl)
			uri.setScheme("wss");
		else
			uri.setScheme("ws");

		log_debug("wsproxysession: %p forwarding to %s:%d", q, qPrintable(target.host), target.port);

		outSock = zhttpManager->createSocket();
		outSock->setParent(this);
		connect(outSock, SIGNAL(connected()), SLOT(out_connected()));
		connect(outSock, SIGNAL(readyRead()), SLOT(out_readyRead()));
		connect(outSock, SIGNAL(framesWritten(int)), SLOT(out_framesWritten(int)));
		connect(outSock, SIGNAL(peerClosed()), SLOT(out_peerClosed()));
		connect(outSock, SIGNAL(closed()), SLOT(out_closed()));
		connect(outSock, SIGNAL(error()), SLOT(out_error()));

		if(target.trusted)
			outSock->setIgnorePolicies(true);

		if(target.insecure)
			outSock->setIgnoreTlsErrors(true);

		outSock->setConnectHost(target.host);
		outSock->setConnectPort(target.port);

		outSock->start(uri, requestData.headers);
	}

	void reject(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		assert(state == Connecting);

		state = Closing;
		inSock->respondError(code, reason, headers, body);
	}

	void reject(int code, const QString &reason, const QString &errorMessage)
	{
		reject(code, reason.toUtf8(), HttpHeaders(), (errorMessage + '\n').toUtf8());
	}

	void tryReadIn()
	{
		while(inSock->framesAvailable() > 0 && outPending < PENDING_FRAMES_MAX)
		{
			ZWebSocket::Frame f = inSock->readFrame();
			outSock->writeFrame(f);
			++outPending;
		}
	}

	void tryReadOut()
	{
		while(outSock->framesAvailable() > 0 && inPending < PENDING_FRAMES_MAX)
		{
			ZWebSocket::Frame f = outSock->readFrame();
			inSock->writeFrame(f);
			++inPending;
		}
	}

	void tryFinish()
	{
		if(!inSock && !outSock)
		{
			cleanup();
			emit q->finishedByPassthrough();
		}
	}

private slots:
	void in_readyRead()
	{
		if(outSock && outSock->state() == ZWebSocket::Connected)
			tryReadIn();
	}

	void in_framesWritten(int count)
	{
		inPending -= count;
		tryReadOut();
	}

	void in_peerClosed()
	{
		if(outSock && outSock->state() != ZWebSocket::Closing)
			outSock->close();
	}

	void in_closed()
	{
		delete inSock;
		inSock = 0;

		if(outSock && outSock->state() != ZWebSocket::Closing)
			outSock->close();

		tryFinish();
	}

	void in_error()
	{
		delete inSock;
		inSock = 0;
		delete outSock;
		outSock = 0;

		tryFinish();
	}

	void out_connected()
	{
		log_debug("wsproxysession: %p connected", q);

		state = Connected;

		inSock->respondSuccess(outSock->responseReason(), outSock->responseHeaders());

		// send any pending frames
		tryReadIn();
	}

	void out_readyRead()
	{
		tryReadOut();
	}

	void out_framesWritten(int count)
	{
		outPending -= count;
		tryReadIn();
	}

	void out_peerClosed()
	{
		if(inSock && inSock->state() != ZWebSocket::Closing)
			inSock->close();
	}

	void out_closed()
	{
		delete outSock;
		outSock = 0;

		if(inSock && inSock->state() != ZWebSocket::Closing)
			inSock->close();

		tryFinish();
	}

	void out_error()
	{
		ZWebSocket::ErrorCondition e = outSock->errorCondition();
		log_debug("wsproxysession: %p target error state=%d, condition=%d", q, (int)state, (int)e);

		if(state == Connecting)
		{
			bool tryAgain = false;

			switch(e)
			{
				case ZWebSocket::ErrorConnect:
				case ZWebSocket::ErrorConnectTimeout:
				case ZWebSocket::ErrorTls:
					tryAgain = true;
					break;
				case ZWebSocket::ErrorRejected:
					reject(outSock->responseCode(), outSock->responseReason(), outSock->responseHeaders(), outSock->responseBody());
					break;
				default:
					reject(502, "Bad Gateway", "Error while proxying to origin.");
					break;
			}

			delete outSock;
			outSock = 0;

			if(tryAgain)
				tryNextTarget();
		}
		else
		{
			delete inSock;
			inSock = 0;
			delete outSock;
			outSock = 0;

			tryFinish();
		}
	}
};

WsProxySession::WsProxySession(ZhttpManager *zhttpManager, DomainMap *domainMap, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, zhttpManager, domainMap);
}

WsProxySession::~WsProxySession()
{
	delete d;
}

void WsProxySession::setDefaultSigKey(const QByteArray &iss, const QByteArray &key)
{
	d->defaultSigIss = iss;
	d->defaultSigKey = key;
}

void WsProxySession::setDefaultUpstreamKey(const QByteArray &key)
{
	d->defaultUpstreamKey = key;
}

void WsProxySession::setUseXForwardedProtocol(bool enabled)
{
	d->useXForwardedProtocol = enabled;
}

void WsProxySession::setXffRules(const XffRule &untrusted, const XffRule &trusted)
{
	d->xffRule = untrusted;
	d->xffTrustedRule = trusted;
}

void WsProxySession::setOrigHeadersNeedMark(const QList<QByteArray> &names)
{
	d->origHeadersNeedMark = names;
}

void WsProxySession::start(ZWebSocket *sock)
{
	d->start(sock);
}

#include "wsproxysession.moc"
