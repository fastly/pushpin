/*
 * Copyright (C) 2014-2015 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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

#ifndef ZRPCREQUEST_H
#define ZRPCREQUEST_H

#include <QVariant>
#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;

class ZrpcRequestPacket;
class ZrpcResponsePacket;
class ZrpcManager;

class ZrpcRequest
{
public:
	enum ErrorCondition
	{
		ErrorGeneric,
		ErrorFormat,
		ErrorUnavailable,
		ErrorTimeout
	};

	ZrpcRequest(ZrpcManager *manager);
	virtual ~ZrpcRequest();

	QByteArray from() const;
	QByteArray id() const;
	QString method() const;
	QVariantHash args() const;
	bool success() const;
	QVariant result() const;
	ErrorCondition errorCondition() const;
	QByteArray errorConditionString() const;

	void start(const QString &method, const QVariantHash &args = QVariantHash());
	void respond(const QVariant &result = QVariant());
	void respondError(const QByteArray &condition, const QVariant &result = QVariant());

	void setError(ErrorCondition condition, const QVariant &result = QVariant());

	Signal finished;
	Signal destroyed;

protected:
	virtual void onSuccess();
	virtual void onError();

private:
	class Private;
	Private *d;

	friend class ZrpcManager;
	ZrpcRequest();
	void setupClient(ZrpcManager *manager);
	void setupServer(ZrpcManager *manager);
	void handle(const QList<QByteArray> &headers, const ZrpcRequestPacket &packet);
	void handle(const ZrpcResponsePacket &packet);
};

#endif
