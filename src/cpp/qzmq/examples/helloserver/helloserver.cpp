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

	void sock_readyRead()
	{
		QZmq::ReqMessage msg = sock.read();
		if(msg.content().isEmpty())
		{
			printf("error: received empty message\n");
			return;
		}

		printf("read: %s\n", msg.content()[0].data());
		QByteArray out = "world";
		printf("writing: %s\n", out.data());
		sock.write(msg.createReply(QList<QByteArray>() << out));
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
