#ifndef INSPECTRESPONSEPACKET_H
#define INSPECTRESPONSEPACKET_H

#include <QVariant>
#include "httpheaders.h"

class InspectResponsePacket
{
public:
	bool noProxy;
	QByteArray sharingKey;
	QVariant userData;

	InspectResponsePacket();

	bool fromVariant(const QVariant &in);
};

#endif
