#ifndef REQUESTSESSION_H
#define REQUESTSESSION_H

#include <QObject>

class M2Request;
class InspectManager;
class InspectData;

class RequestSession : public QObject
{
	Q_OBJECT

public:
	RequestSession(InspectManager *inspectManager, QObject *parent = 0);
	~RequestSession();

	M2Request *request();

	// takes ownership
	void start(M2Request *req);

signals:
	void inspectFinished(const InspectData &idata);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
