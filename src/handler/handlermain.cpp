/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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
#include "handlerapp.h"
#include "rust/bindings.h"

extern "C" {

	int handler_main(const ffi::CliArgsFfi *argsFfi)
	{
		// Create dummy argc/argv for QCoreApplication
		int argc = 1;
		char app_name[] = "pushpin-handler";
		char* argv[] = { app_name, nullptr };
		
		QCoreApplication qapp(argc, argv);

		HandlerApp app;
		return app.run(argsFfi);
	}

}
