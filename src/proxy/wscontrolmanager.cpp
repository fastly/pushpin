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

#include "wscontrolmanager.h"

#include <assert.h>
#include <QPointer>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "log.h"
#include "tnetstring.h"
#include "zutil.h"
#include "wscontrolsession.h"

#define DEFAULT_HWM 5000

class WsControlManager::Private : public QObject
{
	Q_OBJECT

public:
	WsControlManager *q;
	int ipcFileMode;
	QString inSpec;
	QString outSpec;
	QZmq::Socket *inSock;
	QZmq::Socket *outSock;
	QZmq::Valve *inValve;
	QHash<QByteArray, WsControlSession*> sessionsByCid;

	Private(WsControlManager *_q) :
		QObject(_q),
		q(_q),
		ipcFileMode(-1),
		inSock(0),
		outSock(0),
		inValve(0)
	{
	}

	~Private()
	{
	}

	bool setupIn()
	{
		delete inSock;

		inSock = new QZmq::Socket(QZmq::Socket::Pull, this);

		inSock->setHwm(DEFAULT_HWM);

		QString errorMessage;
		if(!ZUtil::setupSocket(inSock, inSpec, true, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		inValve = new QZmq::Valve(inSock, this);
		connect(inValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(in_readyRead(const QList<QByteArray> &)));

		inValve->open();

		return true;
	}

	bool setupOut()
	{
		delete outSock;

		outSock = new QZmq::Socket(QZmq::Socket::Push, this);

		outSock->setHwm(DEFAULT_HWM);
		outSock->setShutdownWaitTime(0);

		QString errorMessage;
		if(!ZUtil::setupSocket(outSock, outSpec, true, ipcFileMode, &errorMessage))
		{
			log_error("%s", qPrintable(errorMessage));
			return false;
		}

		return true;
	}

	void write(const WsControlPacket &packet)
	{
		assert(outSock);

		QByteArray buf = TnetString::fromVariant(packet.toVariant());

		log_debug("wscontrol: OUT %s", buf.data());

		outSock->write(QList<QByteArray>() << buf);
	}

	void write(const WsControlPacket::Item &item)
	{
		WsControlPacket out;
		out.items += item;
		write(out);
	}

private slots:
	void in_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("wscontrol: received message with parts != 1, skipping");
			return;
		}

		log_debug("wscontrol: IN %s", message[0].data());

		QVariant data = TnetString::toVariant(message[0]);
		if(data.isNull())
		{
			log_warning("wscontrol: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		WsControlPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("wscontrol: received message with invalid format (parse failed), skipping");
			return;
		}

		QPointer<QObject> self = this;

		foreach(const WsControlPacket::Item &i, p.items)
		{
			WsControlSession *s = sessionsByCid.value(i.cid);
			if(!s)
			{
				log_warning("wscontrol: received item for unknown connection id, canceling");

				// if this was not an error item, send cancel
				if(i.type != WsControlPacket::Item::Cancel)
				{
					WsControlPacket::Item out;
					out.cid = i.cid;
					out.type = WsControlPacket::Item::Cancel;
					write(out);
				}

				continue;
			}

			s->handle(i);

			if(!self)
				return;
		}
	}
};

WsControlManager::WsControlManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

WsControlManager::~WsControlManager()
{
	delete d;
}

void WsControlManager::setIpcFileMode(int mode)
{
	d->ipcFileMode = mode;
}

bool WsControlManager::setInSpec(const QString &spec)
{
	d->inSpec = spec;
	return d->setupIn();
}

bool WsControlManager::setOutSpec(const QString &spec)
{
	d->outSpec = spec;
	return d->setupOut();
}

WsControlSession *WsControlManager::createSession(const QByteArray &cid)
{
	WsControlSession *s = new WsControlSession;
	s->setup(this, cid);
	return s;
}

void WsControlManager::link(WsControlSession *s, const QByteArray &cid)
{
	d->sessionsByCid.insert(cid, s);
}

void WsControlManager::unlink(const QByteArray &cid)
{
	d->sessionsByCid.remove(cid);
}

bool WsControlManager::canWriteImmediately() const
{
	assert(d->outSock);

	return d->outSock->canWriteImmediately();
}

void WsControlManager::write(const WsControlPacket::Item &item)
{
	d->write(item);
}

#include "wscontrolmanager.moc"
