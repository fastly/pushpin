/*
 * Copyright (C) 2016 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $FANOUT_END_LICENSE$
 */

#include <QCoreApplication>
#include <QTimer>
#include "m2adapterapp.h"

class M2AdapterAppMain : public QObject
{
	Q_OBJECT

public:
	M2AdapterApp *app;

public slots:
	void start()
	{
		app = new M2AdapterApp(this);
		connect(app, &M2AdapterApp::quit, this, &M2AdapterAppMain::app_quit);
		app->start();
	}

	void app_quit(int returnCode)
	{
		delete app;
		QCoreApplication::exit(returnCode);
	}
};

extern "C" {

int m2adapter_main(int argc, char **argv)
{
	QCoreApplication qapp(argc, argv);

	M2AdapterAppMain appMain;
	QTimer::singleShot(0, &appMain, SLOT(start()));
	return qapp.exec();
}

}

#include "m2adaptermain.moc"
