/*
 * Copyright (C) 2014-2015 Fanout, Inc.
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

#include "zrpcrequest.h"

#include <assert.h>
#include <QTimer>
#include "packet/zrpcrequestpacket.h"
#include "packet/zrpcresponsepacket.h"
#include "zrpcmanager.h"
#include "uuidutil.h"
#include "log.h"

class ZrpcRequest::Private : public QObject
{
	Q_OBJECT

public:
	ZrpcRequest *q;
	ZrpcManager *manager;
	QList<QByteArray> reqHeaders;
	QByteArray id;
	QString method;
	QVariantHash args;
	bool success;
	QVariant result;
	ErrorCondition condition;
	QByteArray conditionString;
	QTimer *timer;

	Private(ZrpcRequest *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		success(false),
		condition(ErrorGeneric),
		timer(0)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(timer)
		{
			timer->disconnect(this);
			timer->setParent(0);
			timer->deleteLater();
			timer = 0;
		}

		if(manager)
		{
			manager->unlink(q);
			manager = 0;
		}
	}

	void respond(const QVariant &value)
	{
		ZrpcResponsePacket p;
		p.id = id;
		p.success = true;
		p.value = value;
		manager->write(reqHeaders, p);
	}

	void respondError(const QByteArray &condition, const QVariant &value)
	{
		ZrpcResponsePacket p;
		p.id = id;
		p.success = false;
		p.condition = condition;
		p.value = value;
		manager->write(reqHeaders, p);
	}

	void handle(const QList<QByteArray> &headers, const ZrpcRequestPacket &packet)
	{
		reqHeaders = headers;
		id = packet.id;
		method = packet.method;
		args = packet.args;
	}

	void handle(const ZrpcResponsePacket &packet)
	{
		cleanup();

		success = packet.success;
		if(success)
		{
			result = packet.value;
			q->onSuccess();
		}
		else
		{
			if(packet.condition == "bad-format")
				condition = ErrorFormat;
			else
				condition = ErrorGeneric;

			conditionString = packet.condition;

			result = packet.value;
			q->onError();
		}

		emit q->finished();
	}

private slots:
	void doStart()
	{
		if(!manager->canWriteImmediately())
		{
			success = false;
			condition = ErrorUnavailable;
			conditionString = "service-unavailable";
			cleanup();
			emit q->finished();
			return;
		}

		ZrpcRequestPacket p;
		p.id = id;
		p.method = method;
		p.args = args;

		if(manager->timeout() >= 0)
		{
			timer = new QTimer(this);
			connect(timer, &QTimer::timeout, this, &Private::timer_timeout);
			timer->setSingleShot(true);
			timer->start(manager->timeout());
		}

		manager->write(p);
	}

	void timer_timeout()
	{
		success = false;
		condition = ErrorTimeout;
		conditionString = "timeout";
		cleanup();
		emit q->finished();
	}
};

ZrpcRequest::ZrpcRequest(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZrpcRequest::ZrpcRequest(ZrpcManager *manager, QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
	setupClient(manager);
}

ZrpcRequest::~ZrpcRequest()
{
	delete d;
}

QByteArray ZrpcRequest::id() const
{
	return d->id;
}

QString ZrpcRequest::method() const
{
	return d->method;
}

QVariantHash ZrpcRequest::args() const
{
	return d->args;
}

bool ZrpcRequest::success() const
{
	return d->success;
}

QVariant ZrpcRequest::result() const
{
	return d->result;
}

ZrpcRequest::ErrorCondition ZrpcRequest::errorCondition() const
{
	return d->condition;
}

QByteArray ZrpcRequest::errorConditionString() const
{
	return d->conditionString;
}

void ZrpcRequest::start(const QString &method, const QVariantHash &args)
{
	d->method = method;
	d->args = args;
	QMetaObject::invokeMethod(d, "doStart", Qt::QueuedConnection);
}

void ZrpcRequest::respond(const QVariant &result)
{
	d->respond(result);
}

void ZrpcRequest::respondError(const QByteArray &condition, const QVariant &result)
{
	d->respondError(condition, result);
}

void ZrpcRequest::setError(ErrorCondition condition, const QVariant &result)
{
	d->success = false;
	d->condition = condition;
	d->result = result;
}

void ZrpcRequest::onSuccess()
{
	// by default, do nothing
}

void ZrpcRequest::onError()
{
	// by default, do nothing
}

void ZrpcRequest::setupClient(ZrpcManager *manager)
{
	d->id = UuidUtil::createUuid();
	d->manager = manager;
	d->manager->link(this);
}

void ZrpcRequest::setupServer(ZrpcManager *manager)
{
	d->manager = manager;
}

void ZrpcRequest::handle(const QList<QByteArray> &headers, const ZrpcRequestPacket &packet)
{
	assert(d->manager);

	d->handle(headers, packet);
}

void ZrpcRequest::handle(const ZrpcResponsePacket &packet)
{
	assert(d->manager);

	d->handle(packet);
}

#include "zrpcrequest.moc"
