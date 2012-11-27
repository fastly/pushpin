#include "inspectresponsepacket.h"

InspectResponsePacket::InspectResponsePacket() :
	noProxy(false)
{
}

bool InspectResponsePacket::fromVariant(const QVariant &in)
{
	if(in.type() != QVariant::Hash)
		return false;

	QVariantHash obj = in.toHash();

	if(!obj.contains("id") || obj["id"].type() != QVariant::ByteArray)
		return false;
	id = obj["id"].toByteArray();

	if(!obj.contains("no-proxy") || obj["no-proxy"].type() != QVariant::Bool)
		return false;
	noProxy = obj["no-proxy"].toBool();

	sharingKey.clear();
	if(obj.contains("sharing-key"))
	{
		if(obj["sharing-key"].type() != QVariant::ByteArray)
			return false;

		sharingKey = obj["sharing-key"].toByteArray();
	}

	userData = obj["user-data"];

	return true;
}
