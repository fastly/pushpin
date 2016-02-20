/*
 * Copyright (C) 2012-2016 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <QtGlobal>
#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>
#include "app.h"

class AppMain : public QObject
{
	Q_OBJECT

public:
	App *app;

public slots:
	void start()
	{
		app = new App(this);
		connect(app, SIGNAL(quit()), SLOT(app_quit()));
		app->start();
	}

	void app_quit()
	{
		delete app;
		emit quit();
	}

signals:
	void quit();
};

int main(int argc, char **argv)
{
	QCoreApplication qapp(argc, argv);

	qsrand(QDateTime::currentMSecsSinceEpoch() & 0xffffffff);

	AppMain appMain;
	QObject::connect(&appMain, SIGNAL(quit()), &qapp, SLOT(quit()));
	QTimer::singleShot(0, &appMain, SLOT(start()));
	return qapp.exec();
}

#include "main.moc"
