#ifndef ACCEPTRESPONSEPACKET_H
#define ACCEPTRESPONSEPACKET_H

#include <QVariant>
#include <QPair>
#include <QList>
#include "httprequestdata.h"
#include "inspectresponsepacket.h"
#include "httpresponsedata.h"

class AcceptResponsePacket
{
public:
	typedef QPair<QByteArray, QByteArray> Rid;
	QList<Rid> rids;
	HttpRequestData request;

	bool haveInspectInfo;
	InspectResponsePacket inspectInfo;

	bool haveResponse;
	HttpResponseData response;

	AcceptResponsePacket();

	QVariant toVariant() const;
};

#endif
