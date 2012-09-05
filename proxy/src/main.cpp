#include <QCoreApplication>
#include <QTimer>
#include "app.h"

class AppMain : public QObject
{
	Q_OBJECT

public slots:
	void start()
	{
		App *app = new App(this);
		connect(app, SIGNAL(quit()), SIGNAL(quit()));
		app->start();
	}

signals:
	void quit();
};

int main(int argc, char **argv)
{
	QCoreApplication qapp(argc, argv);
	AppMain appMain;
	QObject::connect(&appMain, SIGNAL(quit()), &qapp, SLOT(quit()));
	QTimer::singleShot(0, &appMain, SLOT(start()));
	return qapp.exec();
}

#include "main.moc"
