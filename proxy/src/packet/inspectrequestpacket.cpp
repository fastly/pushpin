#include "inspectrequestpacket.h"

#include "tnetstring.h"

InspectRequestPacket::InspectRequestPacket()
{
}

QVariant InspectRequestPacket::toVariant() const
{
	QVariantHash obj;
	obj["id"] = id;
	obj["method"] = method.toLatin1();
	obj["path"] = path;

	QVariantList vheaders;
	foreach(const HttpHeader &h, headers)
	{
		QVariantList vheader;
		vheader += h.first;
		vheader += h.second;
		vheaders += QVariant(vheader);
	}

	obj["headers"] = vheaders;

	return obj;
}
