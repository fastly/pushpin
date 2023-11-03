#include <stdio.h>
#include <QCoreApplication>
#include <QTimer>
#include "qzmqsocket.h"

class App : public QObject
{
	Q_OBJECT

private:
	QZmq::Socket sock;

public:
	App() :
		sock(QZmq::Socket::Req)
	{
	}

public slots:
	void start()
	{
		connect(&sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(&sock, SIGNAL(messagesWritten(int)), SLOT(sock_messagesWritten(int)));
		sock.connectToAddress("tcp://localhost:5555");
		QByteArray out = "hello";
		printf("writing: %s\n", out.data());
		sock.write(QList<QByteArray>() << out);
	}

signals:
	void quit();

private slots:
	void sock_readyRead()
	{
		QList<QByteArray> resp = sock.read();
		printf("read: %s\n", resp[0].data());
		emit quit();
	}

	void sock_messagesWritten(int count)
	{
		printf("messages written: %d\n", count);
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

#include "helloclient.moc"
