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

#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include "config.h"

Settings::Settings(const QString &fileName) :
	include(0)
{
	main = new QSettings(fileName, QSettings::IniFormat);

	libdir = valueRaw("global/libdir").toString();
	if(libdir.isEmpty())
	{
		if(QFile::exists("src/pushpin/pushpin.pro"))
		{
			// running in tree
			libdir = QFileInfo("src/pushpin").absoluteFilePath();
		}
		else
		{
			// use compiled value
			libdir = LIBDIR;
		}
	}

	rundir = valueRaw("global/rundir").toString();
	if(rundir.isEmpty())
	{
		// fallback to runner section (deprecated)
		rundir = valueRaw("runner/rundir").toString();
	}

	QString includeFile = value("global/include").toString();
	if(!includeFile.isEmpty())
	{
		// if include is a relative path, then use it relative to the config file location
		QFileInfo fi(includeFile);
		if(fi.isRelative())
			includeFile = QFileInfo(QFileInfo(fileName).absoluteDir(), includeFile).filePath();

		include = new QSettings(includeFile, QSettings::IniFormat);
	}
}

Settings::~Settings()
{
	delete include;
	delete main;
}

QString Settings::resolveVars(const QString &in) const
{
	QString out = in;
	out.replace("{libdir}", libdir);
	out.replace("{rundir}", rundir);
	return out;
}

QVariant Settings::valueRaw(const QString &key, const QVariant &defaultValue) const
{
	if(include)
	{
		if(main->contains(key))
			return main->value(key);
		else
			return include->value(key, defaultValue);
	}
	else
		return main->value(key, defaultValue);
}

QVariant Settings::value(const QString &key, const QVariant &defaultValue) const
{
	QVariant v = valueRaw(key, defaultValue);
	if(v.isValid())
	{
		if(v.type() == QVariant::String)
		{
			v = resolveVars(v.toString());
		}
		else if(v.type() == QVariant::StringList)
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
