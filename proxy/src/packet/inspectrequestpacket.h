#ifndef INSPECTREQUESTPACKET_H
#define INSPECTREQUESTPACKET_H

#include <QVariant>
#include "httpheaders.h"

class InspectRequestPacket
{
public:
	QByteArray id;
	QString method;
	QByteArray path;
	HttpHeaders headers;

	InspectRequestPacket();

	QVariant toVariant() const;
};

#endif
