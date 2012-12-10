#ifndef ZURLREQUEST_H
#define ZURLREQUEST_H

#include <QObject>
#include "packet/httpheaders.h"

class QUrl;

class ZurlResponsePacket;
class ZurlManager;

class ZurlRequest : public QObject
{
	Q_OBJECT

public:
	enum ErrorCondition
	{
		ErrorGeneric,
		ErrorPolicy,
		ErrorConnect,
		ErrorTls,
		ErrorTimeout
	};

	// pair of sender + request id
	typedef QPair<QByteArray, QByteArray> Rid;

	~ZurlRequest();

	Rid rid() const;

	void setConnectHost(const QString &host);

	void start(const QString &method, const QUrl &url, const HttpHeaders &headers);

	// may call this multiple times
	void writeBody(const QByteArray &body);

	void endBody();

	int bytesAvailable() const;
	bool isFinished() const;
	ErrorCondition errorCondition() const;

	int responseCode() const;
	QByteArray responseStatus() const;
	HttpHeaders responseHeaders() const;

	QByteArray readResponseBody(int size = -1); // takes from the buffer

signals:
	void readyRead();
	void bytesWritten(int count);
	void error();

private:
	class Private;
	friend class Private;
	Private *d;

	friend class ZurlManager;
	ZurlRequest(QObject *parent = 0);
	void setup(ZurlManager *manager);
	void handle(const ZurlResponsePacket &packet);
};

#endif
