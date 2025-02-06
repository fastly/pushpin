/*
 * Copyright (C) 2016-2019 Fanout, Inc.
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

#ifndef FILTER_H
#define FILTER_H

#include <QString>
#include <QStringList>
#include <QHash>
#include <QMetaType>
#include <QUrl>
#include <boost/signals2.hpp>

#define MESSAGEFILTERSTACK_SIZE_MAX 5

// 2 timers per zhttprequest
#define TIMERS_PER_MESSAGEFILTERSTACK (2 * MESSAGEFILTERSTACK_SIZE_MAX)

class ZhttpManager;

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
		ResponseContent = 0x04,
	};

	class Context
	{
	public:
		QHash<QString, QString> prevIds;
		QHash<QString, QString> subscriptionMeta;
		QHash<QString, QString> publishMeta;

		// for network access
		ZhttpManager *zhttpOut;
		QUrl currentUri;
		QString route;
		bool trusted;

		Context() :
			zhttpOut(0),
			trusted(false)
		{
		}
	};

	class MessageFilter
	{
	public:
		class Result
		{
		public:
			SendAction sendAction;
			QByteArray content;
			QString errorMessage; // non-null on error
		};

		virtual ~MessageFilter();

		// may emit finished immediately
		virtual void start(const Filter::Context &context, const QByteArray &content = QByteArray()) = 0;

		boost::signals2::signal<void(const Result&)> finished;
	};

	class MessageFilterStack : public MessageFilter
	{
	public:
		MessageFilterStack(const QStringList &filterNames);

		// reimplemented
		virtual void start(const Filter::Context &context, const QByteArray &content = QByteArray());

	private:
		std::vector<std::unique_ptr<MessageFilter>> filters_;
		Filter::Context context_;
		QByteArray content_;
		SendAction lastSendAction_;
		boost::signals2::scoped_connection finishedConnection_;

		void nextFilter();
		void filterFinished(const Result &result);
	};

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
	static MessageFilter *createMessageFilter(const QString &name);
	static QStringList names();
	static Targets targets(const QString &name);

protected:
	Filter(const QString &name = QString());

	void setError(const QString &s) { errorMessage_ = s; }

private:
	QString name_;
	Context context_;
	QString errorMessage_;
};

#endif
