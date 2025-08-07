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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include "qtcompat.h"
#include "config.h"
#include "argsdata.h"

Settings::Settings(const ArgsData *args) :
	include_(0),
	portOffset_(0)
{
	loadSettings(args->configFile);

	if(args->ipcPrefix.isEmpty())
		ipcPrefix_ = args->ipcPrefix;

	if(args->portOffset != -1)
		portOffset_ = args->portOffset;
}

Settings::Settings(const QString &fileName) :
	include_(0),
	portOffset_(0)
{
	loadSettings(fileName);
}

void Settings::loadSettings(const QString &fileName) {
	main_ = new QSettings(fileName, QSettings::IniFormat);

	libdir_ = valueRaw("global/libdir").toString();
	if(libdir_.isEmpty())
	{
		if(QFile::exists("src/bin/pushpin.rs"))
		{
			// running in tree
			libdir_ = QFileInfo("src").absoluteFilePath();
		}
		else
		{
			// use compiled value
			libdir_ = Config::get().libDir;
		}
	}

	rundir_ = valueRaw("global/rundir").toString();
	if(rundir_.isEmpty())
	{
		// fallback to runner section (deprecated)
		rundir_ = valueRaw("runner/rundir").toString();
	}

	ipcPrefix_ = valueRaw("global/ipc_prefix", "pushpin-").toString();
	portOffset_ = valueRaw("global/port_offset", 0).toInt();

	QString includeFile = valueRaw("global/include").toString();

	// if include is exactly "internal.conf", rewrite relative to libdir
	// TODO: remove this hack at next major version
	if(includeFile == "internal.conf")
		includeFile = "{libdir}/internal.conf";

	includeFile = resolveVars(includeFile);

	if(!includeFile.isEmpty())
	{
		// if include is a relative path, then use it relative to the config file location
		QFileInfo fi(includeFile);
		if(fi.isRelative())
			includeFile = QFileInfo(QFileInfo(fileName).absoluteDir(), includeFile).filePath();

		include_ = new QSettings(includeFile, QSettings::IniFormat);
	}
}

Settings::~Settings()
{
	delete include_;
	delete main_;
}

QString Settings::resolveVars(const QString &in) const
{
	QString out = in;
	out.replace("{libdir}", libdir_);
	out.replace("{rundir}", rundir_);
	out.replace("{ipc_prefix}", ipcPrefix_);

	// adjust tcp ports
	int at = 0;
	while(true)
	{
		at = out.indexOf("tcp://", at);
		if(at == -1)
			break;

		at = out.indexOf(':', at + 6);
		if(at == -1)
			break;

		int start = at + 1;
		for(at = start; at < out.length(); ++at)
		{
			if(!out[at].isDigit())
				break;
		}

		bool ok;
		int x = out.mid(start, at - start).toInt(&ok);
		if(!ok)
			break;

		x += portOffset_;

		out.replace(start, at, QString::number(x));
	}

	return out;
}

bool Settings::contains(const QString &key) const
{
	if(main_->contains(key))
		return true;

	if(include_)
		return include_->contains(key);

	return false;
}

QVariant Settings::valueRaw(const QString &key, const QVariant &defaultValue) const
{
	if(include_)
	{
		if(main_->contains(key))
			return main_->value(key);
		else
			return include_->value(key, defaultValue);
	}
	else
		return main_->value(key, defaultValue);
}

QVariant Settings::value(const QString &key, const QVariant &defaultValue) const
{
	QVariant v = valueRaw(key, defaultValue);
	if(v.isValid())
	{
		if(typeId(v) == QMetaType::QString)
		{
			v = resolveVars(v.toString());
		}
		else if(typeId(v) == QMetaType::QStringList)
		{
			QStringList oldList = v.toStringList();
			QStringList newList;
			foreach(QString s, oldList)
				newList += resolveVars(s);
			v = newList;
		}
	}

	return v;
}

int Settings::adjustedPort(const QString &key, int defaultValue) const
{
	int x = value(key, QVariant(defaultValue)).toInt();
	if(x > 0)
		x += portOffset_;
	return x;
}

void Settings::setIpcPrefix(const QString &s)
{
	ipcPrefix_ = s;
}

void Settings::setPortOffset(int x)
{
	portOffset_ = x;
}

bool Settings::operator==(const Settings& other) const {
    return ipcPrefix_ == other.ipcPrefix_ && 
           portOffset_ == other.portOffset_ &&
           libdir_ == other.libdir_ &&
           rundir_ == other.rundir_;
}