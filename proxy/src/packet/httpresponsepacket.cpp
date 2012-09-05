#include "httpresponsepacket.h"

#include "tnetstring.h"

HttpResponsePacket::HttpResponsePacket()
{
}

QByteArray HttpResponsePacket::toM2ByteArray() const
{
	return sender + ' ' + TnetString::fromByteArray(id) + ' ' + data;
}
