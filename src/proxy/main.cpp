/*
 * Copyright (C) 2016 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
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
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#include <QCoreApplication>
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
		connect(app, &App::quit, this, &AppMain::app_quit);
		app->start();
	}

	void app_quit(int returnCode)
	{
		delete app;
		QCoreApplication::exit(returnCode);
	}
};

int main(int argc, char **argv)
{
	QCoreApplication qapp(argc, argv);

	AppMain appMain;
	QTimer::singleShot(0, &appMain, SLOT(start()));
	return qapp.exec();
}

#include "main.moc"
