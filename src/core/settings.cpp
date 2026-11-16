/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include "settings.h"

#include "config.h"
#include "qtcompat.h"
#include "variant.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

Settings::Settings(const CowString &fileName) : include_(0), portOffset_(0) {
    main_ = new QSettings(fileName.asQString(), QSettings::IniFormat);

    libdir_ = valueRaw("global/libdir").toString();
    if (libdir_.isEmpty()) {
        if (QFile::exists("src/bin/pushpin.rs")) {
            // Running in tree
            libdir_ = QFileInfo("src").absoluteFilePath();
        } else {
            // Use compiled value
            libdir_ = Config::get().libDir.asQString();
        }
    }

    rundir_ = valueRaw("global/rundir").toString();
    if (rundir_.isEmpty()) {
        // Fallback to runner section (deprecated)
        rundir_ = valueRaw("runner/rundir").toString();
    }

    ipcPrefix_ = valueRaw("global/ipc_prefix", "pushpin-").toString();
    portOffset_ = valueRaw("global/port_offset", 0).toInt();

    QString includeFile = valueRaw("global/include").toString();

    // If include is exactly "internal.conf", rewrite relative to libdir.
    // TODO: remove this hack at next major version
    if (includeFile == "internal.conf")
        includeFile = "{libdir}/internal.conf";

    includeFile = resolveVars(includeFile);

    if (!includeFile.isEmpty()) {
        // If include is a relative path, then use it relative to the config file location
        QFileInfo fi(includeFile);
        if (fi.isRelative())
            includeFile =
                QFileInfo(QFileInfo(fileName.asQString()).absoluteDir(), includeFile).filePath();

        include_ = new QSettings(includeFile, QSettings::IniFormat);
    }
}

Settings::~Settings() {
    delete include_;
    delete main_;
}

QString Settings::resolveVars(const QString &in) const {
    QString out = in;
    out.replace("{libdir}", libdir_);
    out.replace("{rundir}", rundir_);
    out.replace("{ipc_prefix}", ipcPrefix_);

    // Adjust tcp ports
    int at = 0;
    while (true) {
        at = out.indexOf("tcp://", at);
        if (at == -1)
            break;

        at = out.indexOf(':', at + 6);
        if (at == -1)
            break;

        int start = at + 1;
        for (at = start; at < out.length(); ++at) {
            if (!out[at].isDigit())
                break;
        }

        bool ok;
        int x = out.mid(start, at - start).toInt(&ok);
        if (!ok)
            break;

        x += portOffset_;

        QString port = QString::number(x);
        out.replace(start, at - start, port);
        at = start + port.length();
    }

    return out;
}

bool Settings::contains(const CowString &key) const {
    if (main_->contains(key.asQString()))
        return true;

    if (include_)
        return include_->contains(key.asQString());

    return false;
}

Variant Settings::valueRaw(const CowString &key, const Variant &defaultValue) const {
    if (include_) {
        if (main_->contains(key.asQString()))
            return main_->value(key.asQString());
        else
            return include_->value(key.asQString(), defaultValue);
    } else
        return main_->value(key.asQString(), defaultValue);
}

Variant Settings::value(const CowString &key, const Variant &defaultValue) const {
    Variant v = valueRaw(key, defaultValue);
    if (v.isValid()) {
        if (typeId(v) == VariantType::String) {
            v = resolveVars(v.toString());
        } else if (typeId(v) == VariantType::StringList) {
            QStringList oldList = v.toStringList();
            QStringList newList;
            foreach (QString s, oldList)
                newList += resolveVars(s);
            v = newList;
        }
    }

    return v;
}

int Settings::adjustedPort(const CowString &key, int defaultValue) const {
    int x = value(key, Variant(defaultValue)).toInt();
    if (x > 0)
        x += portOffset_;
    return x;
}

CowString Settings::ipcPrefix() const { return ipcPrefix_; }

int Settings::portOffset() const { return portOffset_; }

void Settings::setIpcPrefix(const CowString &s) { ipcPrefix_ = s.asQString(); }

void Settings::setPortOffset(int x) { portOffset_ = x; }
