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
#include <QTimer>
#include <QDateTime>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include "packet/httprequestdata.h"
#include "log.h"
#include "zhttpmanager.h"
#include "zwebsocket.h"
#include "websocketoverhttp.h"
#include "zroutes.h"
#include "wscontrolmanager.h"
#include "wscontrolsession.h"
#include "xffrule.h"
#include "proxyutil.h"
#include "statsmanager.h"
#include "inspectdata.h"
#include "connectionmanager.h"

#define ACTIVITY_TIMEOUT 60000

class HttpExtension
{
public:
	bool isNull() const { return name.isEmpty(); }

	QByteArray name;
	QHash<QByteArray, QByteArray> params;
};

static int findNext(const QByteArray &in, const char *charList, int start = 0)
{
	int len = qstrlen(charList);
	for(int n = start; n < in.size(); ++n)
	{
		char c = in[n];
		for(int i = 0; i < len; ++i)
		{
			if(c == charList[i])
				return n;
		}
	}

	return -1;
}

static QHash<QByteArray, QByteArray> parseParams(const QByteArray &in, bool *ok = 0)
{
	QHash<QByteArray, QByteArray> out;

	int start = 0;
	while(start < in.size())
	{
		QByteArray var;
		QByteArray val;

		int at = findNext(in, "=;", start);
		if(at != -1)
		{
			var = in.mid(start, at - start).trimmed();
			if(in[at] == '=')
			{
				if(at + 1 >= in.size())
				{
					if(ok)
						*ok = false;
					return QHash<QByteArray, QByteArray>();
				}

				++at;

				if(in[at] == '\"')
				{
					++at;

					bool complete = false;
					for(int n = at; n < in.size(); ++n)
					{
						if(in[n] == '\\')
						{
							if(n + 1 >= in.size())
							{
								if(ok)
									*ok = false;
								return QHash<QByteArray, QByteArray>();
							}

							++n;
							val += in[n];
						}
						else if(in[n] == '\"')
						{
							complete = true;
							at = n + 1;
							break;
						}
						else
							val += in[n];
					}

					if(!complete)
					{
						if(ok)
							*ok = false;
						return QHash<QByteArray, QByteArray>();
					}

					at = in.indexOf(';', at);
					if(at != -1)
						start = at + 1;
					else
						start = in.size();
				}
				else
				{
					int vstart = at;
					at = in.indexOf(';', vstart);
					if(at != -1)
					{
						val = in.mid(vstart, at - vstart).trimmed();
						start = at + 1;
					}
					else
					{
						val = in.mid(vstart).trimmed();
						start = in.size();
					}
				}
			}
			else
				start = at + 1;
		}
		else
		{
			var = in.mid(start).trimmed();
			start = in.size();
		}

		out[var] = val;
	}

	if(ok)
		*ok = true;

	return out;
}

static QByteArray getExtensionRaw(const QList<QByteArray> &extStrings, const QByteArray &name)
{
	foreach(const QByteArray &ext, extStrings)
	{
		int at = ext.indexOf(';');
		if(at != -1)
		{
			if(ext.mid(0, at).trimmed() == name)
				return ext;
		}
		else
		{
			if(ext == name)
				return ext;
		}
	}

	return QByteArray();
}

static HttpExtension getExtension(const QList<QByteArray> &extStrings, const QByteArray &name)
{
	QByteArray ext = getExtensionRaw(extStrings, name);
	if(ext.isNull())
		return HttpExtension();

	HttpExtension e;
	e.name = name;

	int at = ext.indexOf(';');
	if(at != -1)
	{
		bool ok;
		e.params = parseParams(ext.mid(at + 1), &ok);
		if(!ok)
			return HttpExtension();
	}

	return e;
}

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
	ZRoutes *zroutes;
	ZhttpManager *zhttpManager;
	ConnectionManager *connectionManager;
	StatsManager *statsManager;
	WsControlManager *wsControlManager;
	WsControlSession *wsControl;
	QByteArray defaultSigIss;
	QByteArray defaultSigKey;
	QByteArray defaultUpstreamKey;
	bool passToUpstream;
	bool acceptXForwardedProtocol;
	bool useXForwardedProtocol;
	XffRule xffRule;
	XffRule xffTrustedRule;
	QList<QByteArray> origHeadersNeedMark;
	HttpRequestData requestData;
	WebSocket *inSock;
	WebSocket *outSock;
	int inPendingBytes;
	int outPendingBytes;
	int outReadInProgress; // frame type or -1
	QByteArray routeId;
	QByteArray channelPrefix;
	QList<DomainMap::Target> targets;
	DomainMap::Target target;
	bool acceptGripMessages;
	QByteArray messagePrefix;
	bool detached;
	QString subChannel;
	QDateTime activityTime;
	QByteArray publicCid;
	QTimer *keepAliveTimer;

	Private(WsProxySession *_q, ZRoutes *_zroutes, ConnectionManager *_connectionManager, StatsManager *_statsManager, WsControlManager *_wsControlManager) :
		QObject(_q),
		q(_q),
		state(Idle),
		zroutes(_zroutes),
		zhttpManager(0),
		connectionManager(_connectionManager),
		statsManager(_statsManager),
		wsControlManager(_wsControlManager),
		wsControl(0),
		passToUpstream(false),
		acceptXForwardedProtocol(false),
		useXForwardedProtocol(false),
		inSock(0),
		outSock(0),
		inPendingBytes(0),
		outPendingBytes(0),
		outReadInProgress(-1),
		acceptGripMessages(false),
		detached(false),
		keepAliveTimer(0)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		cleanupKeepAliveTimer();

		cleanupInSock();

		delete outSock;
		outSock = 0;

		delete wsControl;
		wsControl = 0;

		if(zhttpManager)
		{
			zroutes->removeRef(zhttpManager);
			zhttpManager = 0;
		}
	}

	void cleanupInSock()
	{
		if(inSock)
		{
			connectionManager->removeConnection(inSock);
			delete inSock;
			inSock = 0;
		}
	}

	void cleanupKeepAliveTimer()
	{
		if(keepAliveTimer)
		{
			keepAliveTimer->disconnect(this);
			keepAliveTimer->setParent(0);
			keepAliveTimer->deleteLater();
			keepAliveTimer = 0;
		}
	}

	void start(WebSocket *sock, const QByteArray &_publicCid, const DomainMap::Entry &entry)
	{
		assert(!inSock);

		state = Connecting;

		publicCid = _publicCid;

		if(statsManager)
			activityTime = QDateTime::currentDateTimeUtc();

		inSock = sock;
		inSock->setParent(this);
		connect(inSock, SIGNAL(readyRead()), SLOT(in_readyRead()));
		connect(inSock, SIGNAL(framesWritten(int, int)), SLOT(in_framesWritten(int, int)));
		connect(inSock, SIGNAL(peerClosed()), SLOT(in_peerClosed()));
		connect(inSock, SIGNAL(closed()), SLOT(in_closed()));
		connect(inSock, SIGNAL(error()), SLOT(in_error()));

		requestData.uri = inSock->requestUri();
		requestData.headers = inSock->requestHeaders();

		QString host = requestData.uri.host();

		if(entry.isNull())
		{
			log_warning("wsproxysession: %p %s has 0 routes", q, qPrintable(host));
			reject(502, "Bad Gateway", QString("No route for host: %1").arg(host));
			return;
		}

		if(!entry.asHost.isEmpty())
			requestData.uri.setHost(entry.asHost);

		QByteArray path = requestData.uri.path(QUrl::FullyEncoded).toUtf8();

		if(entry.pathRemove > 0)
			path = path.mid(entry.pathRemove);

		if(!entry.pathPrepend.isEmpty())
			path = entry.pathPrepend + path;

		requestData.uri.setPath(QString::fromUtf8(path), QUrl::StrictMode);

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

		routeId = entry.id;
		channelPrefix = entry.prefix;
		targets = entry.targets;

		log_debug("wsproxysession: %p %s has %d routes", q, qPrintable(host), targets.count());

		bool trustedClient = ProxyUtil::manipulateRequestHeaders("wsproxysession", q, &requestData, defaultUpstreamKey, entry, sigIss, sigKey, acceptXForwardedProtocol, useXForwardedProtocol, xffTrustedRule, xffRule, origHeadersNeedMark, inSock->peerAddress(), InspectData());

		// don't proxy extensions, as we may not know how to handle them
		requestData.headers.removeAll("Sec-WebSocket-Extensions");

		// send grip extension
		requestData.headers += HttpHeader("Sec-WebSocket-Extensions", "grip");

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

		target = targets.takeFirst();

		QUrl uri = requestData.uri;
		if(target.ssl)
			uri.setScheme("wss");
		else
			uri.setScheme("ws");

		if(!target.host.isEmpty())
			uri.setHost(target.host);

		subChannel = target.subChannel;

		if(zhttpManager)
			zroutes->removeRef(zhttpManager);

		if(target.type == DomainMap::Target::Custom)
		{
			zhttpManager = zroutes->managerForRoute(target.zhttpRoute);
			log_debug("wsproxysession: %p forwarding to %s", q, qPrintable(target.zhttpRoute.baseSpec));
		}
		else // Default
		{
			zhttpManager = zroutes->defaultManager();
			log_debug("wsproxysession: %p forwarding to %s:%d", q, qPrintable(target.connectHost), target.connectPort);
		}

		zroutes->addRef(zhttpManager);

		if(target.overHttp)
		{
			WebSocketOverHttp *woh = new WebSocketOverHttp(zhttpManager, this);
			woh->setConnectionId(publicCid);
			outSock = woh;
		}
		else
		{
			// websockets don't work with zhttp req mode
			if(zhttpManager->clientUsesReq())
			{
				reject(502, "Bad Gateway", "Error while proxying to origin.");
				return;
			}

			outSock = zhttpManager->createSocket();
			outSock->setParent(this);
		}

		connect(outSock, SIGNAL(connected()), SLOT(out_connected()));
		connect(outSock, SIGNAL(readyRead()), SLOT(out_readyRead()));
		connect(outSock, SIGNAL(framesWritten(int, int)), SLOT(out_framesWritten(int, int)));
		connect(outSock, SIGNAL(peerClosed()), SLOT(out_peerClosed()));
		connect(outSock, SIGNAL(closed()), SLOT(out_closed()));
		connect(outSock, SIGNAL(error()), SLOT(out_error()));

		if(target.trusted)
			outSock->setIgnorePolicies(true);

		if(target.insecure)
			outSock->setIgnoreTlsErrors(true);

		if(target.type == DomainMap::Target::Default)
		{
			outSock->setConnectHost(target.connectHost);
			outSock->setConnectPort(target.connectPort);
		}

		outSock->start(uri, requestData.headers);
	}

	void reject(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		assert(state == Connecting);

		logConnection(code, body.size());

		state = Closing;
		inSock->respondError(code, reason, headers, body);
	}

	void reject(int code, const QString &reason, const QString &errorMessage)
	{
		reject(code, reason.toUtf8(), HttpHeaders(), (errorMessage + '\n').toUtf8());
	}

	void tryReadIn()
	{
		while(inSock->framesAvailable() > 0 && ((outSock && outSock->canWrite()) || detached))
		{
			WebSocket::Frame f = inSock->readFrame();

			tryLogActivity();

			if(detached)
				continue;

			outSock->writeFrame(f);
			outPendingBytes += f.data.size();

			restartKeepAlive();
		}
	}

	void tryReadOut()
	{
		while(outSock->framesAvailable() > 0 && ((inSock && inSock->canWrite()) || detached))
		{
			WebSocket::Frame f = outSock->readFrame();

			tryLogActivity();

			if(detached)
				continue;

			if(f.type == WebSocket::Frame::Text || f.type == WebSocket::Frame::Binary || f.type == WebSocket::Frame::Continuation)
			{
				// we are skipping the rest of this message
				if(f.type == WebSocket::Frame::Continuation && outReadInProgress == -1)
					continue;

				if(f.type != WebSocket::Frame::Continuation)
					outReadInProgress = (int)f.type;

				if(wsControl && acceptGripMessages)
				{
					if(f.type == WebSocket::Frame::Text && f.data.startsWith("c:"))
					{
						// grip messages must only be one frame
						if(!f.more)
							wsControl->sendGripMessage(f.data.mid(2)); // process
						else
							outReadInProgress = -1; // ignore rest of message
					}
					else if(f.type != WebSocket::Frame::Continuation && f.data.startsWith(messagePrefix))
					{
						f.data = f.data.mid(messagePrefix.size());
						inSock->writeFrame(f);
						inPendingBytes += f.data.size();

						restartKeepAlive();
					}
					else if(f.type == WebSocket::Frame::Continuation)
					{
						assert(outReadInProgress != -1);

						inSock->writeFrame(f);
						inPendingBytes += f.data.size();

						restartKeepAlive();
					}
				}
				else
				{
					inSock->writeFrame(f);
					inPendingBytes += f.data.size();

					restartKeepAlive();
				}

				if(!f.more)
					outReadInProgress = -1;
			}
			else
			{
				// always relay non-content frames
				inSock->writeFrame(f);
				inPendingBytes += f.data.size();

				restartKeepAlive();
			}
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

	void tryLogActivity()
	{
		if(statsManager && !activityTime.isNull())
		{
			QDateTime now = QDateTime::currentDateTimeUtc();
			if(now >= activityTime.addMSecs(ACTIVITY_TIMEOUT))
			{
				statsManager->addActivity(routeId);

				activityTime = activityTime.addMSecs((activityTime.msecsTo(now) / ACTIVITY_TIMEOUT) * ACTIVITY_TIMEOUT);
			}
		}
	}

	void logConnection(int responseCode, int responseBodySize)
	{
		QString targetStr;
		if(target.type == DomainMap::Target::Custom)
		{
			targetStr = (target.zhttpRoute.req ? "zhttpreq/" : "zhttp/") + target.zhttpRoute.baseSpec;
		}
		else // Default
		{
			targetStr = target.connectHost + ':' + QString::number(target.connectPort);
		}

		QString msg = QString("GET %1 -> %2").arg(inSock->requestUri().toString(QUrl::FullyEncoded), targetStr);
		if(target.overHttp)
			msg += "[http]";
		QUrl ref = QUrl(QString::fromUtf8(inSock->requestHeaders().get("Referer")));
		if(!ref.isEmpty())
			msg += QString(" ref=%1").arg(ref.toString(QUrl::FullyEncoded));
		if(!routeId.isEmpty())
			msg += QString(" route=%1").arg(QString::fromUtf8(routeId));
		msg += QString(" code=%1 %2").arg(QString::number(responseCode), QString::number(responseBodySize));

		log_info("%s", qPrintable(msg));
	}

	void restartKeepAlive()
	{
		if(keepAliveTimer)
			keepAliveTimer->start();
	}

private slots:
	void in_readyRead()
	{
		if((outSock && outSock->state() == WebSocket::Connected) || detached)
			tryReadIn();
	}

	void in_framesWritten(int count, int contentBytes)
	{
		Q_UNUSED(count);

		inPendingBytes -= contentBytes;

		if(!detached && outSock)
			tryReadOut();
	}

	void in_peerClosed()
	{
		if(detached)
		{
			inSock->close();
		}
		else
		{
			if(outSock)
			{
				if(outSock->state() == WebSocket::Connecting)
				{
					delete outSock;
					outSock = 0;

					inSock->close();
				}
				else if(outSock->state() == WebSocket::Connected)
				{
					outSock->close(inSock->peerCloseCode());
				}
			}
		}
	}

	void in_closed()
	{
		int code = inSock->peerCloseCode();
		cleanupInSock();

		if(!detached && outSock && outSock->state() != WebSocket::Closing)
			outSock->close(code);

		tryFinish();
	}

	void in_error()
	{
		cleanupInSock();

		if(!detached)
		{
			delete outSock;
			outSock = 0;
		}

		tryFinish();
	}

	void out_connected()
	{
		log_debug("wsproxysession: %p connected", q);

		state = Connected;

		HttpHeaders headers = outSock->responseHeaders();

		// don't proxy extensions, as we may not know how to handle them
		QList<QByteArray> wsExtensions = headers.takeAll("Sec-WebSocket-Extensions");

		HttpExtension grip = getExtension(wsExtensions, "grip");
		if(!grip.isNull() || !subChannel.isEmpty())
		{
			if(!grip.isNull())
			{
				if(!passToUpstream)
				{
					if(grip.params.contains("message-prefix"))
						messagePrefix = grip.params.value("message-prefix");
					else
						messagePrefix = "m:";

					acceptGripMessages = true;
					log_debug("wsproxysession: %p grip enabled, message-prefix=[%s]", q, messagePrefix.data());
				}
				else
				{
					// tell upstream to do the grip stuff
					headers += HttpHeader("Sec-WebSocket-Extensions", getExtensionRaw(wsExtensions, "grip"));
				}
			}

			if(wsControlManager)
			{
				wsControl = wsControlManager->createSession(publicCid);
				connect(wsControl, SIGNAL(sendEventReceived(WebSocket::Frame::Type, const QByteArray &)), SLOT(wsControl_sendEventReceived(WebSocket::Frame::Type, const QByteArray &)));
				connect(wsControl, SIGNAL(keepAliveSetupEventReceived(bool, int)), SLOT(wsControl_keepAliveSetupEventReceived(bool, int)));
				connect(wsControl, SIGNAL(closeEventReceived(int)), SLOT(wsControl_closeEventReceived(int)));
				connect(wsControl, SIGNAL(detachEventReceived()), SLOT(wsControl_detachEventReceived()));
				connect(wsControl, SIGNAL(cancelEventReceived()), SLOT(wsControl_cancelEventReceived()));
				wsControl->start(routeId, channelPrefix, inSock->requestUri());

				if(!subChannel.isEmpty())
				{
					log_debug("wsproxysession: %p implicit subscription to [%s]", q, qPrintable(subChannel));

					QVariantMap msg;
					msg["type"] = "subscribe";
					msg["channel"] = subChannel;

					QByteArray msgJson = QJsonDocument(QJsonObject::fromVariantMap(msg)).toJson(QJsonDocument::Compact);
					wsControl->sendGripMessage(msgJson);
				}
			}
		}

		inSock->respondSuccess(outSock->responseReason(), headers);

		logConnection(inSock->responseCode(), 0);

		// send any pending frames
		tryReadIn();
	}

	void out_readyRead()
	{
		tryReadOut();
	}

	void out_framesWritten(int count, int contentBytes)
	{
		Q_UNUSED(count);

		outPendingBytes -= contentBytes;

		if(!detached && inSock)
			tryReadIn();
	}

	void out_peerClosed()
	{
		if(!detached && inSock && inSock->state() != WebSocket::Closing)
			inSock->close(outSock->peerCloseCode());
	}

	void out_closed()
	{
		int code = outSock->peerCloseCode();
		delete outSock;
		outSock = 0;

		if(!detached && inSock && inSock->state() != WebSocket::Closing)
			inSock->close(code);

		tryFinish();
	}

	void out_error()
	{
		WebSocket::ErrorCondition e = outSock->errorCondition();
		log_debug("wsproxysession: %p target error state=%d, condition=%d", q, (int)state, (int)e);

		if(detached)
		{
			delete outSock;
			outSock = 0;

			tryFinish();
			return;
		}

		if(state == Connecting)
		{
			bool tryAgain = false;

			switch(e)
			{
				case WebSocket::ErrorConnect:
				case WebSocket::ErrorConnectTimeout:
				case WebSocket::ErrorTls:
					tryAgain = true;
					break;
				case WebSocket::ErrorRejected:
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
			cleanupInSock();

			delete outSock;
			outSock = 0;

			tryFinish();
		}
	}

	void wsControl_sendEventReceived(WebSocket::Frame::Type type, const QByteArray &message)
	{
		// this method accepts a full message, which must be typed
		if(type == WebSocket::Frame::Continuation)
			return;

		// only send if we can, otherwise drop
		if(inSock && inSock->canWrite())
		{
			inSock->writeFrame(WebSocket::Frame(type, message, false));

			inPendingBytes += message.size();
		}
	}

	void wsControl_keepAliveSetupEventReceived(bool enable, int timeout)
	{
		if(enable)
		{
			if(!keepAliveTimer)
			{
				keepAliveTimer = new QTimer(this);
				connect(keepAliveTimer, SIGNAL(timeout()), SLOT(keepAliveTimer_timeout()));
			}

			keepAliveTimer->setInterval(timeout * 1000);
			keepAliveTimer->start();
		}
		else
		{
			cleanupKeepAliveTimer();
		}
	}

	void wsControl_closeEventReceived(int code)
	{
		if(!detached && outSock && outSock->state() != WebSocket::Closing)
			outSock->close();

		if(inSock && inSock->state() != WebSocket::Closing)
			inSock->close(code);
	}

	void wsControl_detachEventReceived()
	{
		// if already detached, do nothing
		if(detached)
			return;

		detached = true;

		if(outSock && outSock->state() != WebSocket::Closing)
			outSock->close();
	}

	void wsControl_cancelEventReceived()
	{
		if(outSock)
		{
			delete outSock;
			outSock = 0;
		}

		cleanupInSock();

		tryFinish();
	}

	void keepAliveTimer_timeout()
	{
		wsControl->sendNeedKeepAlive();
	}
};

WsProxySession::WsProxySession(ZRoutes *zroutes, ConnectionManager *connectionManager, StatsManager *statsManager, WsControlManager *wsControlManager, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, zroutes, connectionManager, statsManager, wsControlManager);
}

WsProxySession::~WsProxySession()
{
	delete d;
}

QByteArray WsProxySession::routeId() const
{
	return d->routeId;
}

QByteArray WsProxySession::cid() const
{
	return d->publicCid;
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

void WsProxySession::setAcceptXForwardedProtocol(bool enabled)
{
	d->acceptXForwardedProtocol = enabled;
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

void WsProxySession::start(WebSocket *sock, const QByteArray &publicCid, const DomainMap::Entry &route)
{
	d->start(sock, publicCid, route);
}

#include "wsproxysession.moc"
