#include "m2manager.h"

#include <QObject>
#include <QStringList>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "packet/m2requestpacket.h"
#include "packet/m2responsepacket.h"
#include "m2request.h"
#include "m2response.h"

#define MAX_PENDING 20

class M2Manager::Private : public QObject
{
	Q_OBJECT

public:
	M2Manager *q;
	QStringList in_specs;
	QStringList inhttps_specs;
	QStringList out_specs;
	QZmq::Socket *in_sock;
	QZmq::Socket *inhttps_sock;
	QZmq::Socket *out_sock;
	QZmq::Valve *in_valve;
	QZmq::Valve *inhttps_valve;
	QHash<M2Request::Rid, M2Request*> reqsByRid;
	QList<M2Request*> pendingReqs;

	Private(M2Manager *_q) :
		QObject(_q),
		q(_q),
		in_sock(0),
		inhttps_sock(0),
		out_sock(0),
		in_valve(0),
		inhttps_valve(0)
	{
	}

	bool setupIncomingPlain()
	{
		if(in_sock)
		{
			delete in_valve;
			in_valve = 0;
			delete in_sock;
			in_sock = 0;
		}

		in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
		foreach(const QString &spec, in_specs)
			in_sock->connectToAddress(spec);

		in_valve = new QZmq::Valve(in_sock, this);
		connect(in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(in_readyRead(const QList<QByteArray> &)));

		in_valve->open();

		return true;
	}

	bool setupIncomingHttps()
	{
		if(inhttps_sock)
		{
			delete inhttps_valve;
			inhttps_valve = 0;
			delete inhttps_sock;
			inhttps_sock = 0;
		}

		inhttps_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
		foreach(const QString &spec, inhttps_specs)
			inhttps_sock->connectToAddress(spec);

		inhttps_valve = new QZmq::Valve(inhttps_sock, this);
		connect(inhttps_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(inhttps_readyRead(const QList<QByteArray> &)));

		inhttps_valve->open();

		return true;
	}

	bool setupOutgoing()
	{
		if(out_sock)
		{
			delete out_sock;
			out_sock = 0;
		}

		out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
		foreach(const QString &spec, out_specs)
			out_sock->connectToAddress(spec);

		return true;
	}

	void handleIncoming(const QList<QByteArray> &message, bool https)
	{
		if(message.count() != 1)
		{
			//log_warning("received message with parts != 1, skipping");
			return;
		}

		//log_info("IN m2 %s", qPrintable(TnetString::byteArrayToEscapedString(msg[0])));

		M2RequestPacket p;
		if(!p.fromByteArray(message[0]))
		{
			//log_warning("received message with invalid format, skipping");
			return;
		}

		M2Request::Rid rid(p.sender, p.id);
		M2Request *req;

		if(p.uploadDone)
		{
			req = reqsByRid.value(rid);
			if(!req)
			{
				// TODO: log_warning, upload finished of unknown request
				return;
			}

			req->uploadDone();
		}
		else
		{
			req = new M2Request;

			reqsByRid.insert(rid, req);

			if(!req->handle(q, p, https))
			{
				reqsByRid.remove(rid);
				delete req;
				// TODO: log warning
				return;
			}

			pendingReqs += req;

			if(pendingReqs.count() >= MAX_PENDING)
			{
				if(in_valve)
					in_valve->close();
				if(inhttps_valve)
					inhttps_valve->close();
			}

			emit q->requestReady();
		}
	}

private slots:
	void in_readyRead(const QList<QByteArray> &message)
	{
		handleIncoming(message, false);
	}

	void inhttps_readyRead(const QList<QByteArray> &message)
	{
		handleIncoming(message, true);
	}
};

M2Manager::M2Manager(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

M2Manager::~M2Manager()
{
	delete d;
}

bool M2Manager::setIncomingPlainSpecs(const QStringList &specs)
{
	d->in_specs = specs;
	return d->setupIncomingPlain();
}

bool M2Manager::setIncomingHttpsSpecs(const QStringList &specs)
{
	d->inhttps_specs = specs;
	return d->setupIncomingHttps();
}

bool M2Manager::setOutgoingSpecs(const QStringList &specs)
{
	d->out_specs = specs;
	return d->setupOutgoing();
}

M2Request *M2Manager::takeNext()
{
	if(d->pendingReqs.isEmpty())
		return 0;

	if(d->in_valve)
		d->in_valve->open();
	if(d->inhttps_valve)
		d->inhttps_valve->open();

	M2Request *req = d->pendingReqs.takeFirst();
	req->activate();
	return req;
}

M2Response *M2Manager::createResponse(const M2Request::Rid &rid)
{
	M2Response *resp = new M2Response;
	resp->handle(this, rid);
	return resp;
}

void M2Manager::unlink(M2Request *req)
{
	d->reqsByRid.remove(req->rid());
}

void M2Manager::writeResponse(const M2ResponsePacket &packet)
{
	d->out_sock->write(QList<QByteArray>() << packet.toByteArray());
}

#include "m2manager.moc"
