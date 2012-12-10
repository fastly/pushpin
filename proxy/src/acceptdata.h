#ifndef ACCEPTDATA_H
#define ACCEPTDATA_H

#include <QList>
#include "packet/httpheaders.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "inspectdata.h"
#include "m2request.h"

class AcceptData
{
public:
	QList<M2Request::Rid> rids;
	HttpRequestData request;

	bool haveInspectData;
	InspectData inspectData;

	bool haveResponse;
	HttpResponseData response;

	AcceptData() :
		haveInspectData(false),
		haveResponse(false)
	{
	}
};

#endif
