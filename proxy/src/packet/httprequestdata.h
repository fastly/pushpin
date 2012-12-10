#ifndef HTTPREQUESTDATA_H
#define HTTPREQUESTDATA_H

#include "httpheaders.h"

class HttpRequestData
{
public:
	QString method;
	QByteArray path;
	HttpHeaders headers;
	QByteArray body;
};

#endif
