#ifndef INSPECTREQUEST_H
#define INSPECTREQUEST_H

#include <QObject>

class HttpRequestData;
class InspectData;
class InspectResponsePacket;
class InspectManager;

class InspectRequest : public QObject
{
	Q_OBJECT

public:
	~InspectRequest();

	void start(const HttpRequestData &hdata);

signals:
	void finished(const InspectData &idata);
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class InspectManager;
	InspectRequest(QObject *parent = 0);
	QByteArray id() const;
	void setup(InspectManager *manager);
	void handle(const InspectResponsePacket &packet);
};

#endif
