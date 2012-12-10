#include "acceptresponsepacket.h"

AcceptResponsePacket::AcceptResponsePacket() :
	haveInspectInfo(false),
	haveResponse(false)
{
}

QVariant AcceptResponsePacket::toVariant() const
{
	QVariantHash obj;

	QVariantList vrids;
	foreach(const Rid &r, rids)
	{
		QVariantHash vrid;
		vrid["sender"] = r.first;
		vrid["id"] = r.second;
		vrids += vrid;
	}

	obj["rids"] = vrids;

	if(haveInspectInfo)
	{
		// TODO
	}

	if(haveResponse)
	{
		QVariantHash vresponse;

		vresponse["code"] = response.code;
		vresponse["status"] = response.status;

		QVariantList vheaders;
		foreach(const HttpHeader &h, response.headers)
		{
			QVariantList vheader;
			vheader += h.first;
			vheader += h.second;
			vheaders += QVariant(vheader);
		}
		vresponse["headers"] = vheaders;

		vresponse["body"] = response.body;

		obj["response"] = vresponse;
	}

        return obj;
}
