#ifndef M2REQUESTPACKET_H
#define M2REQUESTPACKET_H

#include <QVariant>
#include "httpheaders.h"

class M2RequestPacket
{
public:
	QByteArray sender;
	QByteArray id;
	bool isHttps;

	QString method;
	QByteArray path;
	HttpHeaders headers;
	QByteArray body;

	QString uploadFile;
	bool uploadDone;

	M2RequestPacket();

	bool fromByteArray(const QByteArray &in);
};

#endif
