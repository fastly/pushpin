#include "acceptresponsepacket.h"

AcceptResponsePacket::AcceptResponsePacket() :
	inspectInfo(0),
	response(0)
{
}

AcceptResponsePacket::AcceptResponsePacket(const AcceptResponsePacket &from)
{
	*this = from;
}

AcceptResponsePacket::~AcceptResponsePacket()
{
	delete inspectInfo;
	delete response;
}

AcceptResponsePacket & AcceptResponsePacket::operator=(const AcceptResponsePacket &from)
{
	delete inspectInfo;
	inspectInfo = 0;
	delete response;
	response = 0;

	if(from.inspectInfo)
		inspectInfo = new InspectResponsePacket(*from.inspectInfo);
	if(from.response)
		response = new Response(*from.response);

	return *this;
}

QVariant AcceptResponsePacket::toVariant() const
{
	// TODO
	return QVariant();
}
