#include "m2response.h"

#include "packet/m2responsepacket.h"
#include "m2manager.h"

class M2Response::Private
{
public:
	M2Manager *manager;
	M2Request::Rid rid;

	Private() :
		manager(0)
	{
	}
};

M2Response::M2Response(QObject *parent) :
	QObject(parent)
{
	d = new Private;
}

M2Response::~M2Response()
{
	delete d;
}

void M2Response::write(int code, const QString &status, const HttpHeaders &headers, const QByteArray &body)
{
	M2ResponsePacket p;
	p.sender = d->rid.first;
	p.id = d->rid.second;
	p.data = "HTTP/1.1 " + QByteArray::number(code) + ' ' + status.toLatin1() + "\r\n";
	foreach(const HttpHeader &h, headers)
		p.data += h.first + ": " + h.second + "\r\n";
	p.data += "\r\n";
	p.data += body;
	d->manager->writeResponse(p);

	QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection);
}

void M2Response::handle(M2Manager *manager, const M2Request::Rid &rid)
{
	d->manager = manager;
	d->rid = rid;
}
