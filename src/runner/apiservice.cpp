/*
 * Copyright (C) 2026 Fastly, Inc.
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
 */

#include "apiservice.h"

#include "cowurl.h"
#include "log.h"
#include "template.h"
#include <QDir>
#include <QProcess>

ApiService::ApiService(const QString &binFile, const QString &runDir, const QString &logDir,
                       const QString &filePrefix, int logLevel, int bufferSize, int bodyBufferSize,
                       const QList<ListenPort> &ports, const QStringList &itemOutSpecs) {
    args_ += binFile;

    if (!logDir.isEmpty()) {
        setStandardOutputFile(QDir(logDir).filePath(filePrefix + "api.log"));
    }

    if (logLevel >= 0)
        args_ += "--log-level=" + QString::number(logLevel);

    if (bufferSize > 0)
        args_ += "--buffer-size=" + QString::number(bufferSize);

    if (bodyBufferSize > 0)
        args_ += "--body-buffer-size=" + QString::number(bodyBufferSize);

    foreach (const ListenPort &p, ports) {
        if (!p.localPath.isEmpty()) {
            QString arg = "--listen=" + p.localPath + ",local";

            if (p.mode >= 0)
                arg += ",mode=" + QString::number(p.mode, 8);

            if (!p.user.isEmpty())
                arg += ",user=" + p.user;

            if (!p.group.isEmpty())
                arg += ",group=" + p.group;

            args_ += arg;
        } else {
            CowUrl url("http://" + (!p.addr.isNull() ? p.addr.toString() : QString("0.0.0.0")));
            url.setPort(p.port);

            QString arg = "--listen=" + url.authority();

            args_ += arg;
        }
    }

    foreach (const QString &spec, itemOutSpecs) {
        args_ += "--item-out=" + spec;
    }

    setName("api");
    setPidFile(QDir(runDir).filePath(filePrefix + "api.pid"));
}

QStringList ApiService::arguments() const { return args_; }
