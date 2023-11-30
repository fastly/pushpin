/*
 * Copyright (C) 2023 Fastly, Inc.
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

#include "config.h"

#include "rust/build_config.h"

namespace Config {

static Config *g_config = 0;

Config & get()
{
	if(!g_config)
	{
		Config *c = new Config;

		BuildConfig *bc = build_config_new();
		c->version = QString(bc->version);
		c->configDir = QString(bc->config_dir);
		c->libDir = QString(bc->lib_dir);
		build_config_destroy(bc);

		g_config = c;
	}

	return *g_config;
}

}
