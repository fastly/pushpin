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

#ifndef SETTINGS_H
#define SETTINGS_H

#include <QString>
#include <QVariant>

class QSettings;

class Settings
{
public:
	Settings(const QString &fileName);
	~Settings();

	QVariant valueRaw(const QString &key, const QVariant &defaultValue = QVariant()) const;
	QVariant value(const QString &key, const QVariant &defaultValue = QVariant()) const;

private:
	QSettings *main;
	QSettings *include;
	QString libdir;
	QString rundir;

	QString resolveVars(const QString &in) const;
};

#endif
