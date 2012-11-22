#include "m2responsepacket.h"

#include "tnetstring.h"

M2ResponsePacket::M2ResponsePacket()
{
}

QByteArray M2ResponsePacket::toByteArray() const
{
	return sender + ' ' + TnetString::fromByteArray(id) + ' ' + data;
}
