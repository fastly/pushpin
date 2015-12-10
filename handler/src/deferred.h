/*
 * Copyright (C) 2015 Fanout, Inc.
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

#ifndef DEFERRED_H
#define DEFERRED_H

#include <QVariant>
#include <QObject>

class DeferredResult
{
public:
	bool success;
	QVariant value;

	DeferredResult() :
		success(false)
	{
	}

	DeferredResult(bool _success, const QVariant &_value = QVariant()) :
		success(_success),
		value(_value)
	{
	}
};

Q_DECLARE_METATYPE(DeferredResult)

class Deferred : public QObject
{
	Q_OBJECT

public:
	virtual void cancel();

signals:
	void finished(const DeferredResult &result);

protected:
	Deferred(QObject *parent = 0);
	virtual ~Deferred();

	void setFinished(bool ok, const QVariant &value = QVariant());

private slots:
	void doFinish();

private:
	DeferredResult result_;
};

#endif
