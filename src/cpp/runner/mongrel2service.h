/*
 * Copyright (C) 2016-2020 Fanout, Inc.
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

#ifndef MONGREL2SERVICE_H
#define MONGREL2SERVICE_H

#include "service.h"
#include "listenport.h"

class Mongrel2Service : public Service
{
	Q_OBJECT

public:
	Mongrel2Service(
		const QString &binFile,
		const QString &configFile,
		const QString &serverName,
		const QString &runDir,
		const QString &logDir,
		const QString &filePrefix,
		int port,
		bool ssl,
		int logLevel);

	static bool generateConfigFile(const QString &m2shBinFile, const QString &configTemplateFile, const QString &runDir, const QString &logDir, const QString &ipcPrefix, const QString &filePrefix, const QString &certsDir, int clientBufferSize, int maxconn, const QList<ListenPort> &ports, int logLevel);

	// reimplemented

	virtual QStringList arguments() const;
	virtual bool acceptSighup() const;
	virtual bool alwaysLogStatus() const;
	virtual QString formatLogLine(const QString &line) const;

private:
	QStringList args_;
	QString prefix_;
	int logLevel_;

	QString filterLogLine(int, const QString&) const;
};

#endif
