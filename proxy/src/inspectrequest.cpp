#include "inspectrequest.h"

#include <QUuid>
#include "packet/inspectrequestpacket.h"
#include "packet/inspectresponsepacket.h"
#include "packet/httprequestdata.h"
#include "inspectdata.h"
#include "inspectmanager.h"

class InspectRequest::Private : public QObject
{
	Q_OBJECT

public:
	InspectRequest *q;
	InspectManager *manager;
	QByteArray id;

	Private(InspectRequest *_q) :
		QObject(_q),
		q(_q),
		manager(0)
	{
	}
};

InspectRequest::InspectRequest(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

InspectRequest::~InspectRequest()
{
	delete d;
}

void InspectRequest::start(const HttpRequestData &hdata)
{
	InspectRequestPacket p;
	p.id = d->id;
	p.method = hdata.method;
	p.path = hdata.path;
	p.headers = hdata.headers;

	d->manager->write(p);
}

QByteArray InspectRequest::id() const
{
	return d->id;
}

void InspectRequest::setup(InspectManager *manager)
{
	d->id = QUuid::createUuid().toString().toLatin1();
	d->manager = manager;
}

void InspectRequest::handle(const InspectResponsePacket &packet)
{
	InspectData idata;
	idata.doProxy = !packet.noProxy;
	idata.sharingKey = packet.sharingKey;
	// TODO: user data
	emit finished(idata);
}

#include "inspectrequest.moc"
