#ifndef ACCEPTRESPONSEPACKET_H
#define ACCEPTRESPONSEPACKET_H

#include <QVariant>
#include "httprequestpacket.h"
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

	QList<HttpRequestPacket> requests;
	InspectResponsePacket *inspectInfo;
	Response *response;

	AcceptResponsePacket();

	QVariant toVariant() const;
};

#endif
