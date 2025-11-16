/*
 * Copyright (C) 2014-2020 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
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

#ifndef WEBSOCKETOVERHTTP_H
#define WEBSOCKETOVERHTTP_H

#include "websocket.h"
#include <boost/signals2.hpp>
#include <map>

using std::map;
using Signal = boost::signals2::signal<void()>;
using Connection = boost::signals2::scoped_connection;

class ZhttpManager;

/// Implements WebSocket protocol over HTTP long-polling for outgoing backend connections
class WebSocketOverHttp : public WebSocket
{
public:
	class Event
	{
	public:
		QByteArray type;
		QByteArray content;

		Event() = default;

		Event(const QByteArray &_type, const QByteArray &_content = QByteArray()) :
			type(_type),
			content(_content)
		{
		}
	};

	WebSocketOverHttp(ZhttpManager *zhttpManager);
	~WebSocketOverHttp();

	void setConnectionId(const QByteArray &id);
	void setMaxEventsPerRequest(int max);
	void refresh();

	// Disconnection management is thread local
	static void setMaxManagedDisconnects(int max);
	static void clearDisconnectManager();

	// Reimplemented

	virtual QHostAddress peerAddress() const;

	virtual void setConnectHost(const QString &host);
	virtual void setConnectPort(int port);
	virtual void setIgnorePolicies(bool on);
	virtual void setTrustConnectHost(bool on);
	virtual void setIgnoreTlsErrors(bool on);
	virtual void setClientCert(const QString &cert, const QString &key);

	virtual void start(const QUrl &uri, const HttpHeaders &headers);

	virtual void respondSuccess(const QByteArray &reason, const HttpHeaders &headers);
	virtual void respondError(int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body);

	virtual State state() const;
	virtual QUrl requestUri() const;
	virtual HttpHeaders requestHeaders() const;
	virtual int responseCode() const;
	virtual QByteArray responseReason() const;
	virtual HttpHeaders responseHeaders() const;
	virtual QByteArray responseBody() const;
	virtual int framesAvailable() const;
	virtual int writeBytesAvailable() const;
	virtual int peerCloseCode() const;
	virtual QString peerCloseReason() const;
	virtual ErrorCondition errorCondition() const;

	virtual void writeFrame(const Frame &frame);
	virtual Frame readFrame();
	virtual void close(int code = -1, const QString &reason = QString());

	void setHeaders(const HttpHeaders &headers);

	Signal aboutToSendRequest;
	Signal disconnected;

	// Creates events from `frames`. The returned events are guaranteed to
	// represent 0 or more full messages from `frames`, where the first
	// represented frame is a non-continuation frame. This matches the
	// expectations of `removeContentFromFrames()`, in order to enable
	// removing the frames afterwards.
	static QList<Event> framesToEvents(const QList<Frame> &frames, int eventsMax, int contentMax, bool *ok, int *framesRepresented, int *contentRepresented);

	// Remove `count` content bytes from the beginning of `frames`, removing
	// frames (including 0-sized frames) when their content is entirely removed.
	// when a frame is removed from a multipart message, the original type is
	// carried over into the next frame, which means the first frame can't be a
	// continuation.
	// returns the actual number of content bytes removed, which may be less than
	// `count` if there are not enough content bytes available, or if a frame in
	// a multipart message needs to be removed and there is no frame after it to
	// retain the type.
	static int removeContentFromFrames(QList<WebSocket::Frame> *frames, int count);

private:
	class DisconnectManager;
	friend class DisconnectManager;

	WebSocketOverHttp();
	void sendDisconnect();

	class Private;
	friend class Private;
	std::shared_ptr<Private> d;

	static thread_local DisconnectManager *g_disconnectManager;
	static thread_local int g_maxManagedDisconnects;
};

#endif
