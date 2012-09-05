#ifndef HTTPRESPONSEPACKET_H
#define HTTPRESPONSEPACKET_H

#include <QVariant>

class HttpResponsePacket
{
public:
	QByteArray sender;
	QByteArray id;
	QByteArray data;

	HttpResponsePacket();

	QByteArray toM2ByteArray() const;
};

#endif
