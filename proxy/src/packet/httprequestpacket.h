#ifndef HTTPREQUESTPACKET_H
#define HTTPREQUESTPACKET_H

#include <QVariant>
#include "httpheaders.h"

class HttpRequestPacket
{
public:
	QByteArray sender;
	QByteArray id;
	bool isHttps;

	QString method;
	QByteArray path;
	HttpHeaders headers;
	QByteArray body;

	HttpRequestPacket();

	bool fromM2ByteArray(const QByteArray &in);
	bool fromVariant(const QVariant &in);
};

#endif
