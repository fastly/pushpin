/*
 * Copyright (C) 2014 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
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

#ifndef ZRPCMANAGER_H
#define ZRPCMANAGER_H

#include <QByteArray>
#include <QList>
#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;

class ZrpcRequestPacket;
class ZrpcResponsePacket;
class ZrpcRequest;

/// Manages RPC communication over ZeroMQ between Pushpin processes
class ZrpcManager
{
public:
	ZrpcManager();
	~ZrpcManager();

	int timeout() const;

	void setInstanceId(const QByteArray &instanceId);
	void setIpcFileMode(int mode);
	void setBind(bool enable);
	void setTimeout(int ms);
	void setUnavailableOnTimeout(bool enable);

	bool setClientSpecs(const QStringList &specs);
	bool setServerSpecs(const QStringList &specs);

	ZrpcRequest *takeNext();

	bool canWriteImmediately() const;
	void write(const ZrpcRequestPacket &packet);

	Signal requestReady;

private:
	class Private;
	Private *d;

	friend class ZrpcRequest;
	void link(ZrpcRequest *req);
	void unlink(ZrpcRequest *req);
	void write(const QList<QByteArray> &headers, const ZrpcResponsePacket &packet);
};

#endif
