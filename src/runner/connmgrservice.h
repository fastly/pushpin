/*
 * Copyright (C) 2020-2023 Fanout, Inc.
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

#ifndef CONNMGRSERVICE_H
#define CONNMGRSERVICE_H

#include "service.h"
#include "listenport.h"

class ConnmgrService : public Service
{
public:
	ConnmgrService(
		const QString &name,
		const QString &binFile,
		const QString &runDir,
		const QString &logDir,
		const QString &ipcPrefix,
		const QString &filePrefix,
		int logLevel,
		const QString &certsDir,
		int clientBufferSize,
		int maxconn,
		bool allowCompression,
		const QList<ListenPort> &ports,
		bool enableClient);

	static bool hasClientMode(const QString &binFile);

	// reimplemented

	virtual QStringList arguments() const;

private:
	QStringList args_;
};

#endif
