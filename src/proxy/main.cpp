/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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
#include "app.h"
#include "timer.h"
#include "defercall.h"

class AppMain
{
public:
	App *app;

	void start()
	{
		app = new App;
		app->quit.connect(boost::bind(&AppMain::app_quit, this, boost::placeholders::_1));
		app->start();
	}

private:
	void app_quit(int returnCode)
	{
		delete app;
		QCoreApplication::exit(returnCode);
	}
};

extern "C" {

int proxy_main(int argc, char **argv)
{
	QCoreApplication qapp(argc, argv);

	// plenty for the main thread
	Timer::init(100);

	AppMain appMain;
	DeferCall deferCall;
	deferCall.defer([&] { appMain.start(); });
	int ret = qapp.exec();

	// ensure deferred deletes are processed
	QCoreApplication::instance()->sendPostedEvents();

	// deinit here, after all event loop activity has completed
	DeferCall::cleanup();
	Timer::deinit();

	return ret;
}

}
