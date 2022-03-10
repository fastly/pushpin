/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
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
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
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

	bool contains(const QString &key) const;
	QVariant valueRaw(const QString &key, const QVariant &defaultValue = QVariant()) const;
	QVariant value(const QString &key, const QVariant &defaultValue = QVariant()) const;
	int adjustedPort(const QString &key, int defaultValue = -1) const;

	void setIpcPrefix(const QString &s);
	void setPortOffset(int x);

private:
	QSettings *main_;
	QSettings *include_;
	QString libdir_;
	QString rundir_;
	QString ipcPrefix_;
	int portOffset_;

	QString resolveVars(const QString &in) const;
};

#endif
