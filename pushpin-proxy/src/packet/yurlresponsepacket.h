#ifndef YURLRESPONSEPACKET_H
#define YURLRESPONSEPACKET_H

#include <QVariant>
#include "httpheaders.h"

class YurlResponsePacket
{
public:
	QByteArray id;
	int seq;

	bool isError;
	QByteArray condition;

	bool isLast;
	int code;
	QByteArray status;
	HttpHeaders headers;
	QByteArray body;
	QVariant userData;

	YurlResponsePacket();

	QByteArray toByteArray() const;
};

#endif
