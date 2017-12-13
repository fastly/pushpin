/*
 * Copyright (C) 2014-2016 Fanout, Inc.
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
	QList<PendingItem> pending;

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

		clientSock->setSendHwm(OUT_HWM);
		clientSock->setShutdownWaitTime(REQ_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(clientSock, clientSpecs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		clientValve = new QZmq::Valve(clientSock, this);
		connect(clientValve, &QZmq::Valve::readyRead, this, &Private::client_readyRead);

		clientValve->open();

		return true;
	}

	bool setupServer()
	{
		delete serverValve;
		delete serverSock;

		serverSock = new QZmq::Socket(QZmq::Socket::Router, this);

		serverSock->setReceiveHwm(IN_HWM);
		serverSock->setShutdownWaitTime(REP_WAIT_TIME);

		QString errorMessage;
		if(!ZUtil::setupSocket(serverSock, serverSpecs, doBind, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		serverValve = new QZmq::Valve(serverSock, this);
		connect(serverValve, &QZmq::Valve::readyRead, this, &Private::server_readyRead);

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
