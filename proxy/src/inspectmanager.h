#ifndef INSPECTMANAGER_H
#define INSPECTMANAGER_H

#include <QObject>

class InspectRequestPacket;
class InspectRequest;

class InspectManager : public QObject
{
	Q_OBJECT

public:
	InspectManager(QObject *parent = 0);
	~InspectManager();

	bool setSpec(const QString &spec);

	InspectRequest *createRequest();

private:
	class Private;
	Private *d;

	friend class InspectRequest;
	void write(const InspectRequestPacket &packet);
	void unlink(InspectRequest *req);
};

#endif
