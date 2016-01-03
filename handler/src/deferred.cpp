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

#include "deferred.h"

Deferred::Deferred(QObject *parent) :
	QObject(parent)
{
	qRegisterMetaType<DeferredResult>();
}

Deferred::~Deferred()
{
}

void Deferred::cancel()
{
	delete this;
}

void Deferred::setFinished(bool ok, const QVariant &value)
{
	result_.success = ok;
	result_.value = value;

	QMetaObject::invokeMethod(this, "doFinish", Qt::QueuedConnection);
}

void Deferred::doFinish()
{
	emit finished(result_);
	delete this;
}
