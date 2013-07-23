#include <stdio.h>
#include <QSet>
#include <QCoreApplication>
#include <QTimer>
#include <QFile>
#include <QTcpSocket>
#include <QTcpServer>
#include <QUrl>

class Handler : public QObject
{
	Q_OBJECT

private:
	enum State
	{
		ReadHeader,
		ReadBody,
		WriteBody
	};

	QTcpSocket *sock;
	State state;
	QString method;
	QByteArray uri;
	QByteArray headerData;
	QByteArray body;
	int contentLength;
	int confirmedWritten;
	int totalWritten;
	int bodyWritten;

public:
	Handler(QTcpSocket *_sock) :
		sock(_sock),
		state(ReadHeader),
		contentLength(0),
		confirmedWritten(0),
		totalWritten(0),
		bodyWritten(0)
	{
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(sock, SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)));
		connect(sock, SIGNAL(disconnected()), SLOT(sock_disconnected()));
		processIn();
	}

	~Handler()
	{
		delete sock;
	}

signals:
	void finished();

private:
	void processHeaderData()
	{
		QList<QByteArray> lines;

		int at = 0;
		while(at < headerData.size())
		{
			QByteArray line;
			int end = headerData.indexOf("\r\n", at);
			if(end != -1)
			{
				line = headerData.mid(at, end - at);
				at = end + 2;
			}
			else
			{
				line = headerData.mid(at);
				at = headerData.size();
			}

			lines += line;
		}

		method = "GET";
		at = lines[0].indexOf(' ');
		method = QString::fromLatin1(lines[0].mid(0, at));
		++at;
		int end = lines[0].indexOf(' ', at);
		uri = lines[0].mid(at, end - at);

		printf("%s %s\n", qPrintable(method), uri.data());

		for(int n = 1; n < lines.count(); ++n)
		{
			const QByteArray &line = lines[n];
			int at = line.indexOf(": ");
			if(at == -1)
				continue;

			QByteArray name = line.mid(0, at);
			QByteArray val = line.mid(at + 2);

			QByteArray lname = name.toLower();
			if(lname == "content-length")
				contentLength = val.toInt();
			else if(lname == "expect" && val == "100-continue")
			{
				QByteArray rheaderData = "HTTP/1.1 100 Continue\r\n\r\n";
				confirmedWritten += rheaderData.size();
				sock->write(rheaderData);
			}
		}
	}

	void processIn()
	{
		QByteArray buf = sock->readAll();

		if(state == ReadHeader)
		{
			int at = buf.indexOf("\r\n\r\n");
			if(at != -1)
			{
				headerData += buf.mid(0, at);
				body = buf.mid(at + 4);
				processHeaderData();

				if(method == "GET")
				{
					QUrl u = QUrl::fromEncoded(uri, QUrl::StrictMode);
					QFile file(u.path());
					file.open(QFile::ReadOnly);
					body = file.readAll();
					contentLength = body.size();
					state = WriteBody;
					QByteArray rheaderData = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " + QByteArray::number(contentLength) + "\r\n\r\n";
					confirmedWritten += rheaderData.size();
					sock->write(rheaderData);
					sock->write(body);
				}
				else
					state = ReadBody;

				processIn();
			}
			else
				headerData += buf;
		}
		else if(state == ReadBody)
		{
			body += buf;
			if(body.size() > contentLength)
			{
				fprintf(stderr, "error: body overrun (at=%d, expected=%d)\n", (int)body.size(), contentLength);
				emit finished();
				return;
			}

			printf("%d/%d\n", body.size(), contentLength);
			if(body.size() == contentLength)
			{
				state = WriteBody;
				QByteArray rheaderData = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " + QByteArray::number(contentLength) + "\r\n\r\n";
				confirmedWritten += rheaderData.size();
				sock->write(rheaderData);
				sock->write(body);
			}
		}
	}

private slots:
	void sock_readyRead()
	{
		processIn();
	}

	void sock_bytesWritten(qint64 bytes)
	{
		totalWritten += (int)bytes;
		if(totalWritten >= confirmedWritten)
		{
			int written = totalWritten - confirmedWritten;
			confirmedWritten = totalWritten;
			bodyWritten += written;
			if(bodyWritten == contentLength)
			{
				printf("closing\n");
				sock->close();
				emit finished();
			}
		}
	}

	void sock_disconnected()
	{
		emit finished();
	}
};

class App : public QObject
{
	Q_OBJECT

public:
	QTcpServer *server;
	QSet<Handler*> handlers;

public slots:
	void start()
	{
		server = new QTcpServer(this);
		connect(server, SIGNAL(newConnection()), SLOT(server_newConnection()));
		if(server->listen(QHostAddress::Any, 8000))
		{
			printf("listening on port 8000\n");
		}
		else
		{
			printf("failed to bind to port 8000\n");
			emit quit();
		}
	}

signals:
	void quit();

private slots:
	void server_newConnection()
	{
		QTcpSocket *sock = server->nextPendingConnection();
		Handler *handler = new Handler(sock);
		handlers += handler;
		connect(handler, SIGNAL(finished()), SLOT(handler_finished()));
	}

	void handler_finished()
	{
		Handler *handler = (Handler *)sender();
		handlers.remove(handler);
		handler->setParent(0);
		handler->disconnect(this);
		handler->deleteLater();
	}
};

int main(int argc, char **argv)
{
	QCoreApplication qapp(argc, argv);
	App app;
	QObject::connect(&app, SIGNAL(quit()), &qapp, SLOT(quit()));
	QTimer::singleShot(0, &app, SLOT(start()));
	return qapp.exec();
}

#include "echo.moc"
