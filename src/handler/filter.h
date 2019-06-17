/*
 * Copyright (C) 2016-2019 Fanout, Inc.
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

#ifndef FILTER_H
#define FILTER_H

#include <QString>
#include <QStringList>
#include <QHash>

class Filter
{
public:
	enum SendAction
	{
		Send,
		Drop
	};

	enum Targets
	{
		MessageDelivery = 0x01,
		MessageContent  = 0x02,
		ProxyContent    = 0x04,
	};

	class Context
	{
	public:
		QHash<QString, QString> prevIds;
		QHash<QString, QString> subscriptionMeta;
		QHash<QString, QString> publishMeta;
	};

	Filter(const QString &name = QString());
	virtual ~Filter();

	const QString & name() const { return name_; }
	const Context & context() const { return context_; }
	QString errorMessage() const { return errorMessage_; }

	void setContext(const Context &context) { context_ = context; }

	virtual SendAction sendAction() const;

	// return null array on error
	virtual QByteArray update(const QByteArray &data);
	virtual QByteArray finalize();

	QByteArray process(const QByteArray &data);

	static Filter *create(const QString &name);
	static QStringList names();
	static Targets targets(const QString &name);

protected:
	void setError(const QString &s) { errorMessage_ = s; }

private:
	QString name_;
	Context context_;
	QString errorMessage_;
};

#endif
