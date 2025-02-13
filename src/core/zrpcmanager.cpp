/*
 * Copyright (C) 2014-2016 Fanout, Inc.
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

#include "zrpcmanager.h"

#include <assert.h>
#include <QStringList>
#include <QFile>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "qzmqreqmessage.h"
#include "log.h"
#include "tnetstring.h"
#include "packet/zrpcrequestpacket.h"
#include "packet/zrpcresponsepacket.h"
#include "zrpcrequest.h"
#include "zutil.h"

#define OUT_HWM 100
#define IN_HWM 100

#define REQ_WAIT_TIME 0
#define REP_WAIT_TIME 500

#define PENDING_MAX 100

class ZrpcManager::Private : public QObject
{
	Q_OBJECT

public:
	class PendingItem
	{
	public:
		QList<QByteArray> headers;
		ZrpcRequestPacket packet;
	};

	ZrpcManager *q;
	QByteArray instanceId;
	int ipcFileMode;
	bool doBind;
	int timeout;
	QStringList clientSpecs;
	QStringList serverSpecs;
	std::unique_ptr<QZmq::Socket> clientSock;
	std::unique_ptr<QZmq::Socket> serverSock;
	std::unique_ptr<QZmq::Valve> clientValve;
	std::unique_ptr<QZmq::Valve> serverValve;
	QHash<QByteArray, ZrpcRequest*> clientReqsById;
	QList<PendingItem> pending;
	Connection clientValveConnection;
	Connection serverValveConnection;

	Private(ZrpcManager *_q) :
		QObject(_q),
		q(_q),
		ipcFileMode(-1),
		doBind(false),
		timeout(-1)
	{
	}

	~Private()
	{
		assert(clientReqsById.isEmpty());
	}

	bool setupClient()
	{
		clientValveConnection.disconnect();
		clientValve.reset();
		clientSock.reset();

		clientSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Dealer);

		clientSock->setSendHwm(OUT_HWM);
		clientSock->setShutdownWaitTime(REQ_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(clientSock.get(), clientSpecs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		clientValve = std::make_unique<QZmq::Valve>(clientSock.get());
		clientValveConnection = clientValve->readyRead.connect(boost::bind(&Private::client_readyRead, this, boost::placeholders::_1));

		clientValve->open();

		return true;
	}

	bool setupServer()
	{
		serverValveConnection.disconnect();
		serverValve.reset();
		serverSock.reset();

		serverSock = std::make_unique<QZmq::Socket>(QZmq::Socket::Router);

		serverSock->setReceiveHwm(IN_HWM);
		serverSock->setShutdownWaitTime(REP_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(serverSock.get(), serverSpecs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		serverValve = std::make_unique<QZmq::Valve>(serverSock.get());
		serverValveConnection = serverValve->readyRead.connect(boost::bind(&Private::server_readyRead, this, boost::placeholders::_1));

		serverValve->open();

		return true;
	}

	void write(const ZrpcRequestPacket &packet)
	{
		assert(clientSock);

		ZrpcRequestPacket p = packet;
		p.from = instanceId;

		QVariant vpacket = p.toVariant();
		QByteArray buf = TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("zrpc client: OUT %s", qPrintable(TnetString::variantToString(vpacket, -1)));

		clientSock->write(QList<QByteArray>() << QByteArray() << buf);
	}

	void write(const QList<QByteArray> &headers, const ZrpcResponsePacket &packet)
	{
		assert(serverSock);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("zrpc server: OUT %s", qPrintable(TnetString::variantToString(vpacket, -1)));

		QList<QByteArray> message;
		message += headers;
		message += QByteArray();
		message += buf;
		serverSock->write(message);
	}

	void client_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 2)
		{
			log_warning("zrpc client: received message with parts != 2, skipping");
			return;
		}

		if(!message[0].isEmpty())
		{
			log_warning("zrpc client: received message with first part non-empty, skipping");
			return;
		}

		QVariant data = TnetString::toVariant(message[1]);
		if(data.isNull())
		{
			log_warning("zrpc client: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("zrpc client: IN %s", qPrintable(TnetString::variantToString(data, -1)));

		ZrpcResponsePacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zrpc client: received message with invalid format (parse failed), skipping");
			return;
		}

		ZrpcRequest *req = clientReqsById.value(p.id);
		if(!req)
		{
			log_debug("zrpc client: received message for unknown request id, skipping");
			return;
		}

		req->handle(p);
	}

	void server_readyRead(const QList<QByteArray> &message)
	{
		QZmq::ReqMessage req(message);

		if(req.content().count() != 1)
		{
			log_warning("zrpc server: received message with parts != 1, skipping");
			return;
		}

		QVariant data = TnetString::toVariant(req.content()[0]);
		if(data.isNull())
		{
			log_warning("zrpc server: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("zrpc server: IN %s", qPrintable(TnetString::variantToString(data, -1)));

		ZrpcRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("zrpc server: received message with invalid format (parse failed), skipping");
			return;
		}

		PendingItem i;
		i.headers = req.headers();
		i.packet = p;
		pending += i;

		if(pending.count() >= PENDING_MAX)
			serverValve->close();

		q->requestReady();
	}
};

ZrpcManager::ZrpcManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZrpcManager::~ZrpcManager()
{
	delete d;
}

int ZrpcManager::timeout() const
{
	return d->timeout;
}

void ZrpcManager::setInstanceId(const QByteArray &instanceId)
{
	d->instanceId = instanceId;
}

void ZrpcManager::setIpcFileMode(int mode)
{
	d->ipcFileMode = mode;
}

void ZrpcManager::setBind(bool enable)
{
	d->doBind = enable;
}

void ZrpcManager::setTimeout(int ms)
{
	d->timeout = ms;
}

bool ZrpcManager::setClientSpecs(const QStringList &specs)
{
	d->clientSpecs = specs;
	return d->setupClient();
}

bool ZrpcManager::setServerSpecs(const QStringList &specs)
{
	d->serverSpecs = specs;
	return d->setupServer();
}

ZrpcRequest *ZrpcManager::takeNext()
{
	if(d->pending.isEmpty())
		return 0;

	Private::PendingItem i = d->pending.takeFirst();
	ZrpcRequest *req = new ZrpcRequest;
	req->setupServer(this);
	req->handle(i.headers, i.packet);

	d->serverValve->open();

	return req;
}

bool ZrpcManager::canWriteImmediately() const
{
	assert(d->clientSock);

	return d->clientSock->canWriteImmediately();
}

void ZrpcManager::link(ZrpcRequest *req)
{
	d->clientReqsById.insert(req->id(), req);
}

void ZrpcManager::unlink(ZrpcRequest *req)
{
	d->clientReqsById.remove(req->id());
}

void ZrpcManager::write(const ZrpcRequestPacket &packet)
{
	d->write(packet);
}

void ZrpcManager::write(const QList<QByteArray> &headers, const ZrpcResponsePacket &packet)
{
	d->write(headers, packet);
}

#include "zrpcmanager.moc"
