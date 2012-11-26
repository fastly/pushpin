#ifndef M2REQUEST_H
#define M2REQUEST_H

#include <QObject>
#include "packet/httpheaders.h"

class M2Manager;
class M2RequestPacket;

class M2Request : public QObject
{
	Q_OBJECT

public:
	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	~M2Request();

	Rid rid() const;
	bool isHttps() const;
	bool isFinished() const;

	QString method() const;
	QByteArray path() const;
	HttpHeaders headers() const;

	QByteArray read();

	// for streamed input, this is updated as body data is received
	int actualContentLength() const;

signals:
	void readyRead();
	void finished();
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class M2Manager;
	M2Request(QObject *parent = 0);
	bool handle(M2Manager *manager, const M2RequestPacket &packet, bool https);
	void activate();
	void uploadDone();
};

#endif
