/*
 * Copyright (C) 2014 Fanout, Inc.
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

#ifndef ZROUTES_H
#define ZROUTES_H

#include <QObject>
#include "zhttpmanager.h"
#include "domainmap.h"

class ZRoutes : public QObject
{
	Q_OBJECT

public:
	ZRoutes(QObject *parent = 0);
	~ZRoutes();

	void setInstanceId(const QByteArray &id);
	void setDefaultOutSpecs(const QStringList &specs);
	void setDefaultOutStreamSpecs(const QStringList &specs);
	void setDefaultInSpecs(const QStringList &specs);

	void setup(const QList<DomainMap::ZhttpRoute> &routes);

	ZhttpManager *defaultManager();
	ZhttpManager *managerForRoute(const DomainMap::ZhttpRoute &route);

	void addRef(ZhttpManager *zhttpManager);
	void removeRef(ZhttpManager *zhttpManager);

private:
	class Private;
	Private *d;
};

#endif
