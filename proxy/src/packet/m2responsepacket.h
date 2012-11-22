#ifndef M2RESPONSEPACKET_H
#define M2RESPONSEPACKET_H

#include <QVariant>

class M2ResponsePacket
{
public:
	QByteArray sender;
	QByteArray id;
	QByteArray data;

	M2ResponsePacket();

	QByteArray toByteArray() const;
};

#endif
