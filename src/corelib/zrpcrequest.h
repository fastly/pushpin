/*
 * Copyright (C) 2014-2015 Fanout, Inc.
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

#ifndef ZRPCREQUEST_H
#define ZRPCREQUEST_H

#include <QObject>
#include <QVariant>

class ZrpcRequestPacket;
class ZrpcResponsePacket;
class ZrpcManager;

class ZrpcRequest : public QObject
{
	Q_OBJECT

public:
	enum ErrorCondition
	{
		ErrorGeneric,
		ErrorFormat,
		ErrorUnavailable,
		ErrorTimeout
	};

	ZrpcRequest(ZrpcManager *manager, QObject *parent = 0);
	~ZrpcRequest();

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

protected:
	virtual void onSuccess();
	virtual void onError();

signals:
	void finished();

private:
	class Private;
	Private *d;

	friend class ZrpcManager;
	ZrpcRequest(QObject *parent = 0);
	void setupClient(ZrpcManager *manager);
	void setupServer(ZrpcManager *manager);
	void handle(const QList<QByteArray> &headers, const ZrpcRequestPacket &packet);
	void handle(const ZrpcResponsePacket &packet);
};

#endif
