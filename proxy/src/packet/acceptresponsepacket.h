#ifndef ACCEPTRESPONSEPACKET_H
#define ACCEPTRESPONSEPACKET_H

#include <QVariant>
#include "m2requestpacket.h"
#include "inspectresponsepacket.h"

class AcceptResponsePacket
{
public:
	class Response
	{
	public:
		int code;
		QByteArray status;
		HttpHeaders headers;
		QByteArray body;

		Response() :
			code(-1)
		{
		}
	};

	QList<M2RequestPacket> requests;
	InspectResponsePacket *inspectInfo;
	Response *response;

	AcceptResponsePacket();
	AcceptResponsePacket(const AcceptResponsePacket &from);
	~AcceptResponsePacket();
	AcceptResponsePacket & operator=(const AcceptResponsePacket &from);

	QVariant toVariant() const;
};

#endif
