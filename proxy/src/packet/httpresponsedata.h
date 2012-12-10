#ifndef HTTPRESPONSEDATA_H
#define HTTPRESPONSEDATA_H

#include "httpheaders.h"

class HttpResponseData
{
public:
	int code;
	QByteArray status;
	HttpHeaders headers;
	QByteArray body;

	HttpResponseData() :
		code(-1)
	{
	}
};

#endif
