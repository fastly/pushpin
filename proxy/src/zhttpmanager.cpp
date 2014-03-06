/*
 * Copyright (C) 2012-2014 Fanout, Inc.
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

#include "zhttpmanager.h"

#include <assert.h>
#include <QStringList>
#include <QHash>
#include <QPointer>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "log.h"

#define OUT_HWM 100
#define IN_HWM 100
#define DEFAULT_HWM 1000

class ZhttpManager::Private : public QObject
{
	Q_OBJECT

public:
	enum SessionType
	{
		UnknownSession,
		HttpSession,
		WebSocketSession
	};

	ZhttpManager *q;
	QStringList client_out_specs;
	QStringList client_out_stream_specs;
	QStringList client_in_specs;
	QStringList server_in_specs;
	QStringList server_in_stream_specs;
	QStringList server_out_specs;
	QZmq::Socket *client_out_sock;
	QZmq::Socket *client_out_stream_sock;
	QZmq::Socket *client_in_sock;
	QZmq::Socket *server_in_sock;
	QZmq::Socket *server_in_stream_sock;
	QZmq::Socket *server_out_sock;
	QZmq::Valve *server_in_valve;
	QByteArray instanceId;
	QHash<ZhttpRequest::Rid, ZhttpRequest*> clientReqsByRid;
	QHash<ZhttpRequest::Rid, ZhttpRequest*> serverReqsByRid;
	QList<ZhttpRequest*> serverPendingReqs;
	QHash<ZWebSocket::Rid, ZWebSocket*> clientSocksByRid;
	QHash<ZWebSocket::Rid, ZWebSocket*> serverSocksByRid;
	QList<ZWebSocket*> serverPendingSocks;

	Private(ZhttpManager *_q) :
		QObject(_q),
		q(_q),
		client_out_sock(0),
		client_out_stream_sock(0),
		client_in_sock(0),
		server_in_sock(0),
		server_in_stream_sock(0),
		server_out_sock(0),
		server_in_valve(0)
	{
	}

	bool setupClientOut()
	{
		delete client_out_sock;

		client_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
		connect(client_out_sock, SIGNAL(messagesWritten(int)), SLOT(client_out_messagesWritten(int)));

		client_out_sock->setHwm(OUT_HWM);

		foreach(const QString &spec, client_out_specs)
			client_out_sock->connectToAddress(spec);

		return true;
	}

	bool setupClientOutStream()
	{
		delete client_out_stream_sock;

		client_out_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);
		connect(client_out_stream_sock, SIGNAL(messagesWritten(int)), SLOT(client_out_stream_messagesWritten(int)));

		client_out_stream_sock->setWriteQueueEnabled(false);
		client_out_stream_sock->setHwm(DEFAULT_HWM);

		foreach(const QString &spec, client_out_stream_specs)
			client_out_stream_sock->connectToAddress(spec);

		return true;
	}

	bool setupClientIn()
	{
		delete client_in_sock;

		client_in_sock = new QZmq::Socket(QZmq::Socket::Sub, this);
		connect(client_in_sock, SIGNAL(readyRead()), SLOT(client_in_readyRead()));

		client_in_sock->setHwm(DEFAULT_HWM);

		foreach(const QString &spec, client_in_specs)
		{
			client_in_sock->subscribe(instanceId + ' ');
			client_in_sock->connectToAddress(spec);
		}

		return true;
	}

	bool setupServerIn()
	{
		delete server_in_sock;

		server_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);

		server_in_sock->setHwm(IN_HWM);

		foreach(const QString &spec, server_in_specs)
			server_in_sock->connectToAddress(spec);

		server_in_valve = new QZmq::Valve(server_in_sock, this);
		connect(server_in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(server_in_readyRead(const QList<QByteArray> &)));

		server_in_valve->open();

		return true;
	}

	bool setupServerInStream()
	{
		delete server_in_stream_sock;

		server_in_stream_sock = new QZmq::Socket(QZmq::Socket::Dealer, this);
		connect(server_in_stream_sock, SIGNAL(readyRead()), SLOT(server_in_stream_readyRead()));

		server_in_stream_sock->setIdentity(instanceId);
		server_in_stream_sock->setHwm(DEFAULT_HWM);

		foreach(const QString &spec, server_in_stream_specs)
			server_in_stream_sock->connectToAddress(spec);

		return true;
	}

	bool setupServerOut()
	{
		delete server_out_sock;

		server_out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
		connect(server_out_sock, SIGNAL(messagesWritten(int)), SLOT(server_out_messagesWritten(int)));

		server_out_sock->setWriteQueueEnabled(false);
		server_out_sock->setHwm(DEFAULT_HWM);

		foreach(const QString &spec, server_out_specs)
			server_out_sock->connectToAddress(spec);

		return true;
	}

	void tryRespondCancel(SessionType type, const ZhttpRequestPacket &packet)
	{
		assert(!packet.from.isEmpty());

		// if this was not an error packet, send cancel
		if(packet.type != ZhttpRequestPacket::Error && packet.type != ZhttpRequestPacket::Cancel)
		{
			ZhttpResponsePacket out;
			out.from = instanceId;
			out.id = packet.id;
			out.type = ZhttpResponsePacket::Cancel;
			write(type, out, packet.from);
		}
	}

	void write(SessionType type, const ZhttpRequestPacket &packet)
	{
		assert(client_out_sock);
		const char *logprefix = logPrefixForType(type);

		QByteArray buf = QByteArray("T") + TnetString::fromVariant(packet.toVariant());

		log_debug("%s client: OUT %s", logprefix, buf.data());

		client_out_sock->write(QList<QByteArray>() << buf);
	}

	void write(SessionType type, const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
	{
		assert(client_out_stream_sock);
		const char *logprefix = logPrefixForType(type);

		QByteArray buf = QByteArray("T") + TnetString::fromVariant(packet.toVariant());

		log_debug("%s client: OUT %s %s", logprefix, instanceAddress.data(), buf.data());

		QList<QByteArray> msg;
		msg += instanceAddress;
		msg += QByteArray();
		msg += buf;
		client_out_stream_sock->write(msg);
	}

	void write(SessionType type, const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
	{
		assert(server_out_sock);
		const char *logprefix = logPrefixForType(type);

		QByteArray buf = instanceAddress + " T" + TnetString::fromVariant(packet.toVariant());

		log_debug("%s server: OUT %s", logprefix, buf.data());

		server_out_sock->write(QList<QByteArray>() << buf);
	}

	static const char *logPrefixForType(SessionType type)
	{
		switch(type)
		{
			case HttpSession: return "zhttp";
			case WebSocketSession: return "zws";
			default: return "zhttp/zws";
		}
	}

public slots:
	void client_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void client_out_stream_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void client_in_readyRead()
	{
		QPointer<QObject> self = this;

		while(client_in_sock->canRead())
		{
			QList<QByteArray> msg = client_in_sock->read();
			if(msg.count() != 1)
			{
				log_warning("zhttp/zws client: received message with parts != 1, skipping");
				continue;
			}

			log_debug("zhttp/zws client: IN %s", msg[0].data());

			int at = msg[0].indexOf(' ');
			if(at == -1)
			{
				log_warning("zhttp/zws client: received message with invalid format, skipping");
				continue;
			}

			QByteArray receiver = msg[0].mid(0, at);
			QByteArray dataRaw = msg[0].mid(at + 1);
			if(dataRaw.length() < 1 || dataRaw[0] != 'T')
			{
				log_warning("zhttp/zws client: received message with invalid format (missing type), skipping");
				continue;
			}

			QVariant data = TnetString::toVariant(dataRaw.mid(1));
			if(data.isNull())
			{
				log_warning("zhttp/zws client: received message with invalid format (tnetstring parse failed), skipping");
				continue;
			}

			ZhttpResponsePacket p;
			if(!p.fromVariant(data))
			{
				log_warning("zhttp/zws client: received message with invalid format (parse failed), skipping");
				continue;
			}

			// is this for a websocket?
			ZWebSocket *sock = clientSocksByRid.value(ZWebSocket::Rid(instanceId, p.id));
			if(sock)
			{
				sock->handle(p);
				if(!self)
					return;

				continue;
			}

			// is this for an http request?
			ZhttpRequest *req = clientReqsByRid.value(ZhttpRequest::Rid(instanceId, p.id));
			if(req)
			{
				req->handle(p);
				if(!self)
					return;

				continue;
			}

			log_debug("zhttp/zws client: received message for unknown request id, canceling");

			// if this was not an error packet, send cancel
			if(p.type != ZhttpResponsePacket::Error && p.type != ZhttpResponsePacket::Cancel && !p.from.isEmpty())
			{
				ZhttpRequestPacket out;
				out.from = instanceId;
				out.id = p.id;
				out.type = ZhttpRequestPacket::Cancel;
				write(UnknownSession, out, p.from);
			}
		}
	}

	void server_in_readyRead(const QList<QByteArray> &msg)
	{
		if(msg.count() != 1)
		{
			log_warning("zhttp/zws server: received message with parts != 1, skipping");
			return;
		}

		log_debug("zhttp/zws server: IN %s", msg[0].data());

		if(msg[0].length() < 1 || msg[0][0] != 'T')
		{
			log_warning("zhttp/zws server: received message with invalid format (missing type), skipping");
			return;
		}

		QVariant data = TnetString::toVariant(msg[0].mid(1));
		if(data.isNull())
		{
			log_warning("zhttp/zws server: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		ZhttpRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zhttp/zws server: received message with invalid format (parse failed), skipping");
			return;
		}

		if(p.from.isEmpty())
		{
			log_warning("zhttp/zws server: received message without from address, skipping");
			return;
		}

		if(p.uri.scheme() == "wss" || p.uri.scheme() == "ws")
		{
			ZWebSocket::Rid rid(p.from, p.id);

			ZWebSocket *sock = serverSocksByRid.value(rid);
			if(sock)
			{
				log_warning("zws server: received message for existing request id, canceling");
				tryRespondCancel(WebSocketSession, p);
				return;
			}

			sock = new ZWebSocket;
			if(!sock->setupServer(q, p))
			{
				delete sock;
				return;
			}

			serverSocksByRid.insert(rid, sock);
			serverPendingSocks += sock;

			emit q->socketReady();
		}
		else if(p.uri.scheme() == "https" || p.uri.scheme() == "http")
		{
			ZhttpRequest::Rid rid(p.from, p.id);

			ZhttpRequest *req = serverReqsByRid.value(rid);
			if(req)
			{
				log_warning("zhttp server: received message for existing request id, canceling");
				tryRespondCancel(HttpSession, p);
				return;
			}

			req = new ZhttpRequest;
			if(!req->setupServer(q, p))
			{
				delete req;
				return;
			}

			serverReqsByRid.insert(rid, req);
			serverPendingReqs += req;

			emit q->requestReady();
		}
		else
		{
			log_debug("zhttp/zws server: rejecting unsupported scheme: %s", qPrintable(p.uri.scheme()));
			tryRespondCancel(UnknownSession, p);
			return;
		}
	}

	void server_in_stream_readyRead()
	{
		QPointer<QObject> self = this;

		while(server_in_stream_sock->canRead())
		{
			QList<QByteArray> msg = server_in_stream_sock->read();
			if(msg.count() != 2)
			{
				log_warning("zhttp/zws server: received message with parts != 2, skipping");
				continue;
			}

			log_debug("zhttp/zws server: IN stream %s", msg[1].data());

			if(msg[1].length() < 1 || msg[1][0] != 'T')
			{
				log_warning("zhttp/zws server: received message with invalid format (missing type), skipping");
				continue;
			}

			QVariant data = TnetString::toVariant(msg[1].mid(1));
			if(data.isNull())
			{
				log_warning("zhttp/zws server: received message with invalid format (tnetstring parse failed), skipping");
				continue;
			}

			ZhttpRequestPacket p;
			if(!p.fromVariant(data))
			{
				log_warning("zhttp/zws server: received message with invalid format (parse failed), skipping");
				continue;
			}

			// is this for a websocket?
			ZWebSocket *sock = serverSocksByRid.value(ZWebSocket::Rid(p.from, p.id));
			if(sock)
			{
				sock->handle(p);
				if(!self)
					return;

				continue;
			}

			// is this for an http request?
			ZhttpRequest *req = serverReqsByRid.value(ZhttpRequest::Rid(p.from, p.id));
			if(req)
			{
				req->handle(p);
				if(!self)
					return;

				continue;
			}

			log_warning("zhttp/zws server: received message for unknown request id, canceling");

			// if this was not an error packet, send cancel
			if(p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel && !p.from.isEmpty())
			{
				ZhttpResponsePacket out;
				out.from = instanceId;
				out.id = p.id;
				out.type = ZhttpResponsePacket::Cancel;
				write(UnknownSession, out, p.from);
			}
		}
	}

	void server_out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}
};

ZhttpManager::ZhttpManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZhttpManager::~ZhttpManager()
{
	delete d;
}

QByteArray ZhttpManager::instanceId() const
{
	return d->instanceId;
}

void ZhttpManager::setInstanceId(const QByteArray &id)
{
	d->instanceId = id;
}

bool ZhttpManager::setClientOutSpecs(const QStringList &specs)
{
	d->client_out_specs = specs;
	return d->setupClientOut();
}

bool ZhttpManager::setClientOutStreamSpecs(const QStringList &specs)
{
	d->client_out_stream_specs = specs;
	return d->setupClientOutStream();
}

bool ZhttpManager::setClientInSpecs(const QStringList &specs)
{
	d->client_in_specs = specs;
	return d->setupClientIn();
}

bool ZhttpManager::setServerInSpecs(const QStringList &specs)
{
	d->server_in_specs = specs;
	return d->setupServerIn();
}

bool ZhttpManager::setServerInStreamSpecs(const QStringList &specs)
{
	d->server_in_stream_specs = specs;
	return d->setupServerInStream();
}

bool ZhttpManager::setServerOutSpecs(const QStringList &specs)
{
	d->server_out_specs = specs;
	return d->setupServerOut();
}

ZhttpRequest *ZhttpManager::createRequest()
{
	ZhttpRequest *req = new ZhttpRequest;
	req->setupClient(this);
	return req;
}

ZhttpRequest *ZhttpManager::takeNextRequest()
{
	ZhttpRequest *req = 0;

	while(!req)
	{
		if(d->serverPendingReqs.isEmpty())
			return 0;

		req = d->serverPendingReqs.takeFirst();
		if(!d->serverReqsByRid.contains(req->rid()))
		{
			// this means the object was a zombie. clean up and take next
			delete req;
			req = 0;
			continue;
		}
	}

	req->startServer();
	return req;
}

ZWebSocket *ZhttpManager::createSocket()
{
	ZWebSocket *sock = new ZWebSocket;
	sock->setupClient(this);
	return sock;
}

ZWebSocket *ZhttpManager::takeNextSocket()
{
	ZWebSocket *sock = 0;

	while(!sock)
	{
		if(d->serverPendingSocks.isEmpty())
			return 0;

		sock = d->serverPendingSocks.takeFirst();
		if(!d->serverSocksByRid.contains(sock->rid()))
		{
			// this means the object was a zombie. clean up and take next
			delete sock;
			sock = 0;
			continue;
		}
	}

	sock->startServer();
	return sock;
}

ZhttpRequest *ZhttpManager::createRequestFromState(const ZhttpRequest::ServerState &state)
{
	ZhttpRequest *req = new ZhttpRequest;
	req->setupServer(this, state);
	return req;
}

void ZhttpManager::link(ZhttpRequest *req)
{
	if(req->isServer())
		d->serverReqsByRid.insert(req->rid(), req);
	else
		d->clientReqsByRid.insert(req->rid(), req);
}

void ZhttpManager::unlink(ZhttpRequest *req)
{
	if(req->isServer())
		d->serverReqsByRid.remove(req->rid());
	else
		d->clientReqsByRid.remove(req->rid());
}

void ZhttpManager::link(ZWebSocket *sock)
{
	if(sock->isServer())
		d->serverSocksByRid.insert(sock->rid(), sock);
	else
		d->clientSocksByRid.insert(sock->rid(), sock);
}

void ZhttpManager::unlink(ZWebSocket *sock)
{
	if(sock->isServer())
		d->serverSocksByRid.remove(sock->rid());
	else
		d->clientSocksByRid.remove(sock->rid());
}

bool ZhttpManager::canWriteImmediately() const
{
	assert(d->client_out_sock);

	return d->client_out_sock->canWriteImmediately();
}

void ZhttpManager::writeHttp(const ZhttpRequestPacket &packet)
{
	d->write(Private::HttpSession, packet);
}

void ZhttpManager::writeHttp(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::HttpSession, packet, instanceAddress);
}

void ZhttpManager::writeHttp(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::HttpSession, packet, instanceAddress);
}

void ZhttpManager::writeWs(const ZhttpRequestPacket &packet)
{
	d->write(Private::WebSocketSession, packet);
}

void ZhttpManager::writeWs(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::WebSocketSession, packet, instanceAddress);
}

void ZhttpManager::writeWs(const ZhttpResponsePacket &packet, const QByteArray &instanceAddress)
{
	d->write(Private::WebSocketSession, packet, instanceAddress);
}

#include "zhttpmanager.moc"
