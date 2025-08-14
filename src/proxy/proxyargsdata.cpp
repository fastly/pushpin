/*
 * Copyright (C) 2015-2022 Fanout, Inc.
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

#include "proxyargsdata.h"
#include "settings.h"
#include "config.h"
#include "log.h"
#include "rust/bindings.h"
#include <QCoreApplication>
#include <QFile>

ProxyArgsData::ProxyArgsData(const ffi::ProxyCliArgs *argsFfi)
{
    configFile  = QString::fromUtf8(argsFfi->config_file);
    logFile     = QString::fromUtf8(argsFfi->log_file);
    logLevel 	= argsFfi->log_level;
    ipcPrefix 	= QString::fromUtf8(argsFfi->ipc_prefix);
    routeLines = QStringList();
    for (unsigned int i = 0; i < argsFfi->routes_count; ++i)
    {
        routeLines << QString::fromUtf8(argsFfi->routes[i]);
    }
    quietCheck  = argsFfi->quiet_check == 1;

    // Set the log level
	if(logLevel != -1)
        log_setOutputLevel(logLevel);
    else
        log_setOutputLevel(LOG_LEVEL_INFO);

    // Set the log file if specified
    if(!logFile.isEmpty())
    {
        if(!log_setFile(logFile))
        {
            log_error("failed to open log file: %s", qPrintable(logFile));
            throw std::exception();
        }
    }

    log_debug("starting...");

    // QSettings doesn't inform us if the config file can't be opened, so do that ourselves
    {
        QFile file(configFile);
        if(!file.open(QIODevice::ReadOnly))
        {
            log_error("failed to open %s", qPrintable(configFile));
            throw std::exception();
        }
    }
}