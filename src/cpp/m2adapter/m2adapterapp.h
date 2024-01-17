/*
 * Copyright (C) 2016 Fanout, Inc.
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

#ifndef M2ADAPTERAPP_H
#define M2ADAPTERAPP_H

#include <QObject>
#include <boost/signals2.hpp>

using std::map;
using SignalInt = boost::signals2::signal<void(int)>;
using Connection = boost::signals2::scoped_connection;

class M2AdapterApp : public QObject
{
	Q_OBJECT

public:
	M2AdapterApp(QObject *parent = 0);
	~M2AdapterApp();

	void start();

	SignalInt quit;

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
