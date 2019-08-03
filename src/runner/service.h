/*
 * Copyright (C) 2016 Fanout, Inc.
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

#ifndef SERVICE_H
#define SERVICE_H

#include <QObject>
#include <QStringList>

class Service : public QObject
{
	Q_OBJECT

public:
	Service(QObject *parent = 0);
	~Service();

	QString name() const;

	virtual QStringList arguments() const = 0;
	virtual bool acceptSighup() const;
	virtual bool isStarted() const;

	virtual bool preStart();
	virtual void start();
	virtual void postStart();
	virtual void stop();
	virtual void postStop();
	virtual QString formatLogLine(const QString &line) const;

	void sendSighup();

protected:
	void setName(const QString &name);
	void setStandardOutputFile(const QString &file);
	void setPidFile(const QString &file);

signals:
	void started();
	void stopped();
	void logLine(const QString &line);
	void error(const QString &message);

private:
	class Private;
	Private *d;
};

#endif
