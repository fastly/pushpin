#include "zurlrequest.h"

#include <QUuid>
#include <QUrl>
#include "packet/zurlrequestpacket.h"
#include "packet/zurlresponsepacket.h"
#include "zurlmanager.h"

#define IDEAL_CREDITS 200000

class ZurlRequest::Private : public QObject
{
	Q_OBJECT

public:
	ZurlRequest *q;
	ZurlManager *manager;
	ZurlRequest::Rid rid;
	QByteArray replyAddress;
	QString connectHost;
	QString method;
	QUrl url;
	HttpHeaders headers;
	QByteArray out;
	int outCredits;
	bool outFinished;
	bool haveResponseValues;
	int responseCode;
	QByteArray responseStatus;
	HttpHeaders responseHeaders;
	QByteArray in;
	bool finished;
	ZurlRequest::ErrorCondition errorCondition;

	Private(ZurlRequest *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		outCredits(0),
		outFinished(false),
		haveResponseValues(false),
		finished(false)
	{
	}

	~Private()
	{
		if(manager)
			manager->unlink(q);
	}

public slots:
	void doStart()
	{
		ZurlRequestPacket p;
		p.id = rid.second;
		p.sender = rid.first;
		p.seq = 0;
		p.method = method;
		p.url = url;
		p.headers = headers;
		//p.more = true;
		//p.stream = true;
		p.connectHost = connectHost;
		p.credits = IDEAL_CREDITS;
		manager->write(p);
	}
};

ZurlRequest::ZurlRequest(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

ZurlRequest::~ZurlRequest()
{
	delete d;
}

ZurlRequest::Rid ZurlRequest::rid() const
{
	return d->rid;
}

void ZurlRequest::setConnectHost(const QString &host)
{
	d->connectHost = host;
}

void ZurlRequest::start(const QString &method, const QUrl &url, const HttpHeaders &headers)
{
	d->method = method;
	d->url = url;
	d->headers = headers;

	QMetaObject::invokeMethod(d, "doStart", Qt::QueuedConnection);
}

void ZurlRequest::writeBody(const QByteArray &body)
{
	d->out += body;
}

void ZurlRequest::endBody()
{
	d->outFinished = true;
}

int ZurlRequest::bytesAvailable() const
{
	return d->in.size();
}

bool ZurlRequest::isFinished() const
{
	return d->finished;
}

ZurlRequest::ErrorCondition ZurlRequest::errorCondition() const
{
	return d->errorCondition;
}

int ZurlRequest::responseCode() const
{
	return d->responseCode;
}

QByteArray ZurlRequest::responseStatus() const
{
	return d->responseStatus;
}

HttpHeaders ZurlRequest::responseHeaders() const
{
	return d->responseHeaders;
}

QByteArray ZurlRequest::readResponseBody(int size)
{
	QByteArray out = d->in.mid(0, size);
	d->in.clear();
	return out;
}

void ZurlRequest::setup(ZurlManager *manager)
{
	d->manager = manager;
	d->rid = Rid(manager->clientId(), QUuid::createUuid().toString().toLatin1());
	d->manager->link(this);
}

void ZurlRequest::handle(const ZurlResponsePacket &packet)
{
	if(!packet.replyAddress.isEmpty())
		d->replyAddress = packet.replyAddress;

	// TODO: isError, condition

	if(!packet.body.isNull())
	{
		if(!d->haveResponseValues)
		{
			d->haveResponseValues = true;

			d->responseCode = packet.code;
			d->responseStatus = packet.status;
			d->responseHeaders = packet.headers;
		}

		d->in += packet.body;

		d->finished = true;

		emit readyRead();
	}

	if(packet.credits != -1)
	{
		d->outCredits += packet.credits;
		//d->tryWrite();
	}
}

#include "zurlrequest.moc"
