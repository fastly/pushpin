#ifndef APP_H
#define APP_H

#include <QObject>

class App : public QObject
{
	Q_OBJECT

public:
	App(QObject *parent = 0);
	~App();

	void start();

signals:
	void quit();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
