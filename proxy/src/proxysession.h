#ifndef PROXYSESSION_H
#define PROXYSESSION_H

#include <QObject>

class AcceptData;
class ZurlManager;
class DomainMap;
class RequestSession;

class ProxySession : public QObject
{
	Q_OBJECT

public:
	ProxySession(ZurlManager *zurlManager, DomainMap *domainMap, QObject *parent = 0);
	~ProxySession();

	// takes ownership
	void add(RequestSession *rs);

signals:
	void finishedByPassthrough();
	void finishedForAccept(const AcceptData &adata);

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
