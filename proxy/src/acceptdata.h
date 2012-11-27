#ifndef ACCEPTDATA_H
#define ACCEPTDATA_H

#include <QList>
#include "packet/httpheaders.h"
#include "httprequestdata.h"
#include "m2request.h"

class AcceptData
{
public:
	QList<M2Request::Rid> rids;
	HttpRequestData request;

	bool haveInspectData;
	InspectData inspectData;

	bool haveResponse;
	int responseCode;
	QByteArray responseStatus;
	HttpHeaders responseHeaders;
	QByteArray responseBody;

	AcceptData() :
		haveInspectData(false),
		haveResponse(false),
		responseCode(-1)
	{
	}
};

#endif
