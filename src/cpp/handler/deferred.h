/*
 * Copyright (C) 2015 Fanout, Inc.
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
	virtual ~Deferred();

	virtual void cancel();

signals:
	void finished(const DeferredResult &result);

protected:
	Deferred(QObject *parent = 0);

	void setFinished(bool ok, const QVariant &value = QVariant());

private slots:
	void doFinish();

private:
	DeferredResult result_;
};

#endif
