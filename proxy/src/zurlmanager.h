#ifndef ZURLMANAGER_H
#define ZURLMANAGER_H

#include <QObject>

class ZurlRequestPacket;
class ZurlRequest;

class ZurlManager : public QObject
{
	Q_OBJECT

public:
	ZurlManager(QObject *parent = 0);
	~ZurlManager();

	QByteArray clientId() const;

	void setClientId(const QByteArray &id);
	bool setOutgoingSpecs(const QStringList &specs);
	bool setOutgoingStreamSpecs(const QStringList &specs);
	bool setIncomingSpecs(const QStringList &specs);

	ZurlRequest *createRequest();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZurlRequest;
	void link(ZurlRequest *req);
	void unlink(ZurlRequest *req);
	void write(const ZurlRequestPacket &packet);
	void write(const ZurlRequestPacket &packet, const QByteArray &instanceAddress);
};

#endif
