#ifndef M2RESPONSE_H
#define M2RESPONSE_H

#include <QObject>
#include "packet/httpheaders.h"
#include "m2request.h"

class M2Manager;

class M2Response : public QObject
{
	Q_OBJECT

public:
	~M2Response();

	void write(int code, const QByteArray &status, const HttpHeaders &headers, const QByteArray &body);
	void write(const QByteArray &body);

signals:
	//void finished();
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class M2Manager;
	M2Response(QObject *parent = 0);
	void handle(M2Manager *manager, const M2Request::Rid &rid);
};

#endif
