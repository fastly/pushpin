#include "zurlmanager.h"

#include <stdio.h>
#include <QStringList>
#include <QHash>
#include <QPointer>
#include "qzmqsocket.h"
#include "packet/tnetstring.h"
#include "packet/zurlrequestpacket.h"
#include "packet/zurlresponsepacket.h"
#include "zurlrequest.h"

class ZurlManager::Private : public QObject
{
	Q_OBJECT

public:
	ZurlManager *q;
	QStringList out_specs;
	QStringList out_stream_specs;
	QStringList in_specs;
	QZmq::Socket *out_sock;
	QZmq::Socket *out_stream_sock;
	QZmq::Socket *in_sock;
	QByteArray clientId;
	QHash<ZurlRequest::Rid, ZurlRequest*> reqsByRid;

	Private(ZurlManager *_q) :
		QObject(_q),
		q(_q),
		out_sock(0),
		out_stream_sock(0),
		in_sock(0)
	{
	}

	bool setupOutgoing()
	{
		delete out_sock;

		out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
		connect(out_sock, SIGNAL(messagesWritten(int)), SLOT(out_messagesWritten(int)));
		foreach(const QString &spec, out_specs)
			out_sock->connectToAddress(spec);

		return true;
	}

	bool setupOutgoingStream()
	{
		delete out_stream_sock;

		out_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);
		connect(out_stream_sock, SIGNAL(messagesWritten(int)), SLOT(out_stream_messagesWritten(int)));
		foreach(const QString &spec, out_stream_specs)
			out_stream_sock->connectToAddress(spec);

		return true;
	}

	bool setupIncoming()
	{
		delete in_sock;

		in_sock = new QZmq::Socket(QZmq::Socket::Sub, this);
		connect(in_sock, SIGNAL(readyRead()), SLOT(in_readyRead()));
		foreach(const QString &spec, in_specs)
		{
			in_sock->subscribe(clientId);
			in_sock->connectToAddress(spec);
		}

		return true;
	}

public slots:
	void out_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void out_stream_messagesWritten(int count)
	{
		Q_UNUSED(count);
	}

	void in_readyRead()
	{
		QPointer<QObject> self = this;

		while(in_sock->canRead())
		{
			QList<QByteArray> msg = in_sock->read();
			if(msg.count() != 1)
			{
				// TODO: log warning, invalid
				continue;
			}

			int at = msg[0].indexOf(' ');
			if(at == -1)
			{
				// TODO: log warning, invalid
				continue;
			}

			QByteArray receiver = msg[0].mid(0, at);
			QVariant data = TnetString::toVariant(msg[0].mid(at + 1));
			if(data.isNull())
			{
				// TODO: log warning, invalid
				continue;
			}

			ZurlResponsePacket p;
			if(!p.fromVariant(data))
			{
				// TODO: log warning, invalid
				continue;
			}

			ZurlRequest *req = reqsByRid.value(ZurlRequest::Rid(clientId, p.id));
			if(!req)
			{
				// TODO: log warning, unknown request id
				continue;
			}

			req->handle(p);

			if(!self)
				return;
		}
	}
};

ZurlManager::ZurlManager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZurlManager::~ZurlManager()
{
	delete d;
}

QByteArray ZurlManager::clientId() const
{
	return d->clientId;
}

void ZurlManager::setClientId(const QByteArray &id)
{
	d->clientId = id;
}

bool ZurlManager::setOutgoingSpecs(const QStringList &specs)
{
	d->out_specs = specs;
	return d->setupOutgoing();
}

bool ZurlManager::setOutgoingStreamSpecs(const QStringList &specs)
{
	d->out_stream_specs = specs;
	return d->setupOutgoingStream();
}

bool ZurlManager::setIncomingSpecs(const QStringList &specs)
{
	d->in_specs = specs;
	return d->setupIncoming();
}

ZurlRequest *ZurlManager::createRequest()
{
	ZurlRequest *req = new ZurlRequest;
	req->setup(this);
	return req;
}

void ZurlManager::link(ZurlRequest *req)
{
	d->reqsByRid.insert(req->rid(), req);
}

void ZurlManager::unlink(ZurlRequest *req)
{
	d->reqsByRid.remove(req->rid());
}

void ZurlManager::write(const ZurlRequestPacket &packet)
{
	d->out_sock->write(QList<QByteArray>() << TnetString::fromVariant(packet.toVariant()));
}

void ZurlManager::write(const ZurlRequestPacket &packet, const QByteArray &instanceAddress)
{
	QList<QByteArray> msg;
	msg += instanceAddress;
	msg += QByteArray();
	msg += TnetString::fromVariant(packet.toVariant());
	d->out_stream_sock->write(msg);
}

#include "zurlmanager.moc"
