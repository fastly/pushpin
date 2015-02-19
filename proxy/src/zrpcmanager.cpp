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

#include "zrpcmanager.h"

#include <assert.h>
#include <QStringList>
#include <QFile>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "log.h"
#include "tnetstring.h"
#include "packet/zrpcrequestpacket.h"
#include "packet/zrpcresponsepacket.h"
#include "zrpcrequest.h"
#include "zutil.h"

#define DEFAULT_HWM 5000
#define SHUTDOWN_WAIT_TIME 1000

class ZrpcManager::Private : public QObject
{
	Q_OBJECT

public:
	ZrpcManager *q;
	int ipcFileMode;
	bool doBind;
	int timeout;
	QStringList clientSpecs;
	QStringList serverSpecs;
	QZmq::Socket *clientSock;
	QZmq::Socket *serverSock;
	QZmq::Valve *clientValve;
	QZmq::Valve *serverValve;
	QHash<QByteArray, ZrpcRequest*> clientReqsById;
	QList<ZrpcRequestPacket> pending;

	Private(ZrpcManager *_q) :
		QObject(_q),
		q(_q),
		ipcFileMode(-1),
		doBind(false),
		timeout(-1),
		clientSock(0),
		serverSock(0),
		clientValve(0),
		serverValve(0)
	{
	}

	~Private()
	{
		assert(clientReqsById.isEmpty());
	}

	bool setupClient()
	{
		delete clientValve;
		delete clientSock;

		clientSock = new QZmq::Socket(QZmq::Socket::Dealer, this);

		clientSock->setHwm(DEFAULT_HWM);
		clientSock->setShutdownWaitTime(SHUTDOWN_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(clientSock, clientSpecs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		clientValve = new QZmq::Valve(clientSock, this);
		connect(clientValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(client_readyRead(const QList<QByteArray> &)));

		clientValve->open();

		return true;
	}

	bool setupServer()
	{
		delete serverValve;
		delete serverSock;

		serverSock = new QZmq::Socket(QZmq::Socket::Rep, this);

		serverSock->setHwm(DEFAULT_HWM);
		serverSock->setShutdownWaitTime(SHUTDOWN_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(serverSock, serverSpecs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		serverValve = new QZmq::Valve(serverSock, this);
		connect(serverValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(server_readyRead(const QList<QByteArray> &)));

		serverValve->open();

		return true;
	}

	void write(const ZrpcRequestPacket &packet)
	{
		assert(clientSock);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("zrpc client: OUT %s", qPrintable(TnetString::variantToString(vpacket, -1)));

		clientSock->write(QList<QByteArray>() << QByteArray() << buf);
	}

	void write(const ZrpcResponsePacket &packet)
	{
		assert(serverSock);

		QVariant vpacket = packet.toVariant();
		QByteArray buf = TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("zrpc server: OUT %s", qPrintable(TnetString::variantToString(vpacket, -1)));

		serverSock->write(QList<QByteArray>() << buf);
	}

private slots:
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
			log_warning("zrpc client: received message for unknown request id, skipping");
			return;
		}

		req->handle(p);
	}

	void server_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("zrpc server: received message with parts != 1, skipping");
			return;
		}

		QVariant data = TnetString::toVariant(message[0]);
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

		pending += p;

		emit q->requestReady();
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

	ZrpcRequestPacket p = d->pending.takeFirst();
	ZrpcRequest *req = new ZrpcRequest;
	req->setupServer(this);
	req->handle(p);
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

void ZrpcManager::write(const ZrpcResponsePacket &packet)
{
	d->write(packet);
}

#include "zrpcmanager.moc"
