/*
 * Copyright (C) 2012-2022 Fanout, Inc.
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

#ifndef SETTINGS_H
#define SETTINGS_H

#include <QString>
#include <QVariant>
#include "rust/bindings.h"

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
	bool operator==(const Settings& other) const;

private:
	QSettings *main_;
	QSettings *include_;
	QString libdir_;
	QString rundir_;
	QString ipcPrefix_;
	int portOffset_;

	QString resolveVars(const QString &in) const;
};

Settings loadArgs(const ffi::CCliArgsFfi *args);

#endif
