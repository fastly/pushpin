#include <stdio.h>
#include <QCoreApplication>
#include <QTimer>
#include "qzmqreqmessage.h"
#include "qzmqreprouter.h"

class App : public QObject
{
	Q_OBJECT

private:
	QZmq::RepRouter sock;

	void sock_messagesWritten(int count)
	{
		printf("messages written: %d\n", count);
	}

public slots:
	void start()
	{
		rrConnection = sock.readyRead.connect(boost::bind(&Private::sock_readyRead, this));
		mwConnection = sock.messagesWritten.connect(boost::bind(&Private::sock_messagesWritten, this, boost::placeholders::_1));
		sock.bind("tcp://*:5555");
	}

signals:
	void quit();
};

int main(int argc, char **argv)
{
	QCoreApplication qapp(argc, argv);
	App app;
	QObject::connect(&app, SIGNAL(quit()), &qapp, SLOT(quit()));
	QTimer::singleShot(0, &app, SLOT(start()));
	return qapp.exec();
}

#include "helloserver.moc"
