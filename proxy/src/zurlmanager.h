#ifndef ZURLMANAGER_H
#define ZURLMANAGER_H

#include <QObject>

class ZurlRequest;

class ZurlManager : public QObject
{
	Q_OBJECT

public:
	ZurlManager(QObject *parent = 0);
	~ZurlManager();

	bool setOutgoingSpecs(const QStringList &specs);
	bool setOutgoingStreamSpecs(const QStringList &specs);
	bool setIncomingSpecs(const QStringList &specs);

	ZurlRequest *createRequest();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZurlRequest;
	//void unlink(M2Request *req);
	//void writeResponse(const M2ResponsePacket &packet);
};

#endif
