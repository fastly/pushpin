#ifndef M2MANAGER_H
#define M2MANAGER_H

#include <QObject>
#include "m2request.h"

class M2ResponsePacket;
class M2Response;

class M2Manager : public QObject
{
	Q_OBJECT

public:
	M2Manager(QObject *parent = 0);
	~M2Manager();

	bool setIncomingPlainSpecs(const QStringList &specs);
	bool setIncomingHttpsSpecs(const QStringList &specs);
	bool setOutgoingSpecs(const QStringList &specs);

	M2Request *takeNext();
	M2Response *createResponse(const M2Request::Rid &rid);

signals:
	void requestReady();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class M2Request;
	friend class M2Response;
	void unlink(M2Request *req);
	void writeResponse(const M2ResponsePacket &packet);
};

#endif
