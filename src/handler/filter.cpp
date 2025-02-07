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

#include "filter.h"

#include <QJsonDocument>
#include <QJsonObject>
#include "log.h"
#include "format.h"
#include "idformat.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"

namespace {

class SkipSelfFilter : public Filter, public Filter::MessageFilter
{
public:
	SkipSelfFilter() :
		Filter("skip-self")
	{
	}

	virtual void start(const Filter::Context &context, const QByteArray &content)
	{
		setContext(context);

		Result r;
		r.sendAction = sendAction();
		r.content = content;
		finished(r);
	}

	virtual SendAction sendAction() const
	{
		QString user = context().subscriptionMeta.value("user");
		QString sender = context().publishMeta.value("sender");
		if(!user.isEmpty() && !sender.isEmpty() && sender == user)
			return Drop;

		return Send;
	}
};

class SkipUsersFilter : public Filter, public Filter::MessageFilter
{
public:
	SkipUsersFilter() :
		Filter("skip-users")
	{
	}

	virtual void start(const Filter::Context &context, const QByteArray &content)
	{
		setContext(context);

		Result r;
		r.sendAction = sendAction();
		r.content = content;
		finished(r);
	}

	virtual SendAction sendAction() const
	{
		QString user = context().subscriptionMeta.value("user");

		QStringList skip_users;
		foreach(const QString &part, context().publishMeta.value("skip_users").split(','))
		{
			QString s = part.trimmed();
			if(!s.isEmpty())
				skip_users += s;
		}

		if(!user.isEmpty() && skip_users.contains(user))
			return Drop;

		return Send;
	}
};

class RequireSubFilter : public Filter, public Filter::MessageFilter
{
public:
	RequireSubFilter() :
		Filter("require-sub")
	{
	}

	virtual void start(const Filter::Context &context, const QByteArray &content)
	{
		setContext(context);

		Result r;
		r.sendAction = sendAction();
		r.content = content;
		finished(r);
	}

	virtual SendAction sendAction() const
	{
		QString require_sub = context().publishMeta.value("require_sub");
		if(!require_sub.isEmpty() && !context().prevIds.keys().contains(require_sub))
			return Drop;

		return Send;
	}
};

class BuildIdFilter : public Filter, public Filter::MessageFilter
{
public:
	IdFormat::ContentRenderer *idContentRenderer;

	BuildIdFilter() :
		Filter("build-id"),
		idContentRenderer(0)
	{
	}

	~BuildIdFilter()
	{
		delete idContentRenderer;
	}

	bool ensureInit()
	{
		if(!idContentRenderer)
		{
			QString idFormat = context().subscriptionMeta.value("id_format");
			if(idFormat.isNull())
			{
				setError("no sub meta 'id_format'");
				return false;
			}

			QHash<QString, QByteArray> idTemplateVars;
			QHashIterator<QString, QString> it(context().prevIds);
			while(it.hasNext())
			{
				it.next();
				idTemplateVars.insert(it.key(), it.value().toUtf8());
			}

			QString _error;
			QByteArray id;

			if(!idTemplateVars.isEmpty())
			{
				id = IdFormat::renderId(idFormat.toUtf8(), idTemplateVars, &_error);
				if(id.isNull())
				{
					setError(QString("failed to render ID: %1").arg(_error));
					return false;
				}
			}

			bool hex = false;

			QString idEncoding = context().subscriptionMeta.value("id_encoding");
			if(!idEncoding.isNull())
			{
				if(idEncoding == "hex")
				{
					hex = true;
				}
				else
				{
					setError(QString("unsupported encoding: %1").arg(idEncoding));
					return false;
				}
			}

			idContentRenderer = new IdFormat::ContentRenderer(id, hex);
		}

		return true;
	}

	virtual void start(const Filter::Context &context, const QByteArray &content)
	{
		setContext(context);

		Result r;
		r.sendAction = sendAction();
		r.content = process(content);
		finished(r);
	}

	virtual QByteArray update(const QByteArray &data)
	{
		if(!ensureInit())
			return QByteArray();

		QByteArray buf = idContentRenderer->update(data);
		if(buf.isNull())
		{
			setError(idContentRenderer->errorMessage());
			return QByteArray();
		}

		return buf;
	}

	virtual QByteArray finalize()
	{
		if(!ensureInit())
			return QByteArray();

		QByteArray buf = idContentRenderer->finalize();
		if(buf.isNull())
		{
			setError(idContentRenderer->errorMessage());
			return QByteArray();
		}

		return buf;
	}
};

class VarSubstFormatHandler : public Format::Handler
{
public:
	QHash<QString, QString> vars;

	virtual QByteArray handle(char type, const QByteArray &arg, QString *error) const
	{
		if(type != 's')
		{
			*error = QString("Unknown directive '%1'").arg(type);
			return QByteArray();
		}

		if(arg.isNull())
		{
			*error = QString("Directive 's' requires argument");
			return QByteArray();
		}

		QString value = vars.value(arg);
		if(value.isNull())
		{
			*error = QString("No such variable '%1'").arg(QString::fromUtf8(arg));
			return QByteArray();
		}

		return value.toUtf8();
	}
};

class VarSubstFilter : public Filter, public Filter::MessageFilter
{
public:
	VarSubstFilter() :
		Filter("var-subst")
	{
	}

	virtual void start(const Filter::Context &context, const QByteArray &content)
	{
		setContext(context);

		Result r;
		r.sendAction = sendAction();
		r.content = process(content);
		finished(r);
	}

	virtual QByteArray update(const QByteArray &data)
	{
		VarSubstFormatHandler handler;
		handler.vars = context().subscriptionMeta;

		QString errorMessage;
		QByteArray buf = Format::process(data, &handler, 0, &errorMessage);
		if(buf.isNull())
		{
			setError(errorMessage);
			return QByteArray();
		}

		return buf;
	}

	virtual QByteArray finalize()
	{
		return QByteArray("");
	}
};

enum HttpFilterMode
{
	Check,
	Modify
};

class HttpFilterInner
{
public:
	HttpFilterMode mode;
	std::unique_ptr<ZhttpRequest> req;
	QUrl uri;
	HttpHeaders headers;
	QByteArray origContent;
	bool haveResponseHeader;
	QByteArray responseBody;

	boost::signals2::signal<void(const Filter::MessageFilter::Result&)> finished;

	HttpFilterInner(HttpFilterMode _mode) :
		mode(_mode),
		haveResponseHeader(false)
	{
	}

	void setup(ZhttpManager *zhttpOut, const QUrl &_uri, const HttpHeaders &_headers, const QVariant &passthroughData, const QByteArray &content)
	{
		uri = _uri;
		headers = _headers;
		origContent = content;

		req.reset(zhttpOut->createRequest());

		// safe to not track, since req can't outlive this
		req->readyRead.connect(boost::bind(&HttpFilterInner::req_readyRead, this));
		req->error.connect(boost::bind(&HttpFilterInner::req_error, this));

		req->setPassthroughData(passthroughData);
	}

	void startRequest()
	{
		req->start("POST", uri, headers);

		if(mode == Modify)
			req->writeBody(origContent);

		req->endBody();
	}

	void req_readyRead()
	{
		if(!haveResponseHeader)
		{
			haveResponseHeader = true;

			int code = req->responseCode();
			switch(code)
			{
				case 200:
				case 204:
					break;
				default:
					Filter::MessageFilter::Result r;
					r.errorMessage = QString("unexpected network request status: code=%1").arg(code);
					finished(r);
					return;
			}
		}

		QByteArray body = req->readBody();

		if(mode == Modify)
			responseBody += body;

		if(!req->isFinished())
			return;

		Filter::MessageFilter::Result r;

		if(req->responseHeaders().get("Action") == "drop")
		{
			// drop
			r.sendAction = Filter::Drop;
		}
		else
		{
			// accept
			r.sendAction = Filter::Send;

			switch(mode)
			{
				case Check:
					// as-is
					r.content = origContent;
					break;
				case Modify:
					switch(req->responseCode())
					{
						case 204:
							// as-is
							r.content = origContent;
							break;
						default:
							// replace content
							r.content = responseBody;
							break;
					}
					break;
			}
		}

		finished(r);
	}

	void req_error()
	{
		Filter::MessageFilter::Result r;
		r.errorMessage = "network request failed";
		finished(r);
	}
};

class HttpFilter : public Filter::MessageFilter
{
public:
	std::shared_ptr<HttpFilterInner> inner;
	boost::signals2::scoped_connection finishedConnection;

	class RequestAction : public RateLimiter::Action
	{
	public:
		std::weak_ptr<HttpFilterInner> inner;

		RequestAction(const std::shared_ptr<HttpFilterInner> &_inner) :
			inner(_inner)
		{
		}

		virtual bool execute()
		{
			auto target = inner.lock();
			if(!target)
				return false;

			target->startRequest();
			return true;
		}
	};

	HttpFilter(HttpFilterMode mode)
	{
		inner = std::make_shared<HttpFilterInner>(mode);

		finishedConnection = inner->finished.connect(boost::bind(&HttpFilter::inner_finished, this, boost::placeholders::_1));
	}

	virtual void start(const Filter::Context &context, const QByteArray &content)
	{
		QUrl url = QUrl(context.subscriptionMeta.value("url"), QUrl::StrictMode);
		if(!url.isValid())
		{
			Result r;
			r.errorMessage = "invalid or missing url value";
			finished(r);
			return;
		}

		QUrl currentUri = context.currentUri;
		if(currentUri.scheme() == "wss")
			currentUri.setScheme("https");
		else if(currentUri.scheme() == "ws")
			currentUri.setScheme("http");

		QUrl destUri = currentUri.resolved(url);

		int currentPort = currentUri.port(currentUri.scheme() == "https" ? 443 : 80);
		int destPort = destUri.port(destUri.scheme() == "https" ? 443 : 80);

		QVariantHash passthroughData;

		passthroughData["route"] = context.route.toUtf8();

		// if dest link points to the same service as the current request,
		//   then we can assume the network would send the request back to
		//   us, so we can handle it internally. if the link points to a
		//   different service, then we can't make this assumption and need
		//   to make the request over the network. note that such a request
		//   could still end up looping back to us
		if(destUri.scheme() == currentUri.scheme() && destUri.host() == currentUri.host() && destPort == currentPort)
		{
			// tell the proxy that we prefer the request to be handled
			//   internally, using the same route
			passthroughData["prefer-internal"] = true;
		}

		// needed in case internal routing is not used
		if(context.trusted)
			passthroughData["trusted"] = true;

		HttpHeaders headers;

		{
			QVariantMap vmap;
			QHashIterator<QString, QString> it(context.subscriptionMeta);
			while(it.hasNext())
			{
				it.next();
				vmap[it.key()] = it.value();
			}

			QJsonDocument doc = QJsonDocument(QJsonObject::fromVariantMap(vmap));
			headers += HttpHeader("Sub-Meta", doc.toJson(QJsonDocument::Compact));
		}

		{
			QVariantMap vmap;
			QHashIterator<QString, QString> it(context.publishMeta);
			while(it.hasNext())
			{
				it.next();
				vmap[it.key()] = it.value();
			}

			QJsonDocument doc = QJsonDocument(QJsonObject::fromVariantMap(vmap));
			headers += HttpHeader("Pub-Meta", doc.toJson(QJsonDocument::Compact));
		}

		{
			QHashIterator<QString, QString> it(context.prevIds);
			while(it.hasNext())
			{
				it.next();
				const QString &name = it.key();
				const QString &prevId = it.value();

				if(!prevId.isNull())
					headers += HttpHeader("Grip-Last", name.toUtf8() + "; last-id=" + prevId.toUtf8());
			}
		}

		inner->setup(context.zhttpOut, destUri, headers, passthroughData, content);

		QString key = QString::fromUtf8(destUri.toEncoded());

		assert(context.limiter);

		if(!context.limiter->addAction(key, new RequestAction(inner)))
		{
			// the limiter shouldn't have an hwm, but let's handle the error
			// here in case one is ever added

			Result r;
			r.errorMessage = "network request limit reached";
			finished(r);
			return;
		}
	}

	void inner_finished(const Result &r)
	{
		finished(r);
	}
};

}

Filter::MessageFilter::~MessageFilter() = default;

Filter::Filter(const QString &name) :
	name_(name)
{
}

Filter::~Filter() = default;

Filter::SendAction Filter::sendAction() const
{
	return Send;
}

QByteArray Filter::update(const QByteArray &data)
{
	return data;
}

QByteArray Filter::finalize()
{
	return QByteArray("");
}

QByteArray Filter::process(const QByteArray &data)
{
	QByteArray out = update(data);
	if(out.isNull())
		return QByteArray();

	QByteArray buf = finalize();
	if(buf.isNull())
		return QByteArray();

	return out + buf;
}

Filter *Filter::create(const QString &name)
{
	if(name == "skip-self")
		return new SkipSelfFilter;
	else if(name == "skip-users")
		return new SkipUsersFilter;
	else if(name == "require-sub")
		return new RequireSubFilter;
	else if(name == "build-id")
		return new BuildIdFilter;
	else if(name == "var-subst")
		return new VarSubstFilter;
	else
		return 0;
}

Filter::MessageFilter *Filter::createMessageFilter(const QString &name)
{
	if(name == "skip-self")
		return new SkipSelfFilter;
	else if(name == "skip-users")
		return new SkipUsersFilter;
	else if(name == "require-sub")
		return new RequireSubFilter;
	else if(name == "build-id")
		return new BuildIdFilter;
	else if(name == "var-subst")
		return new VarSubstFilter;
	else if(name == "http-check")
		return new HttpFilter(Check);
	else if(name == "http-modify")
		return new HttpFilter(Modify);
	else
		return 0;
}

QStringList Filter::names()
{
	return (QStringList()
		<< "skip-self"
		<< "skip-users"
		<< "require-sub"
		<< "build-id"
		<< "var-subst"
		<< "http-check"
		<< "http-modify");
}

Filter::Targets Filter::targets(const QString &name)
{
	if(name == "skip-self")
		return Filter::MessageDelivery;
	else if(name == "skip-users")
		return Filter::MessageDelivery;
	else if(name == "require-sub")
		return Filter::MessageDelivery;
	else if(name == "build-id")
		return Filter::Targets(Filter::MessageContent | Filter::ResponseContent);
	else if(name == "var-subst")
		return Filter::MessageContent;
	else if(name == "http-check")
		return Filter::MessageDelivery;
	else if(name == "http-modify")
		return Filter::Targets(Filter::MessageDelivery | Filter::MessageContent);
	else
		return Filter::Targets(0);
}

Filter::MessageFilterStack::MessageFilterStack(const QStringList &filterNames)
{
	foreach(const QString &name, filterNames)
	{
		MessageFilter *f = createMessageFilter(name);
		if(f)
			filters_.emplace_back(std::unique_ptr<MessageFilter>(f));
	}
}

void Filter::MessageFilterStack::start(const Filter::Context &context, const QByteArray &content)
{
	context_ = context;
	content_ = content;
	lastSendAction_ = Send;

	nextFilter();
}

void Filter::MessageFilterStack::nextFilter()
{
	if(filters_.empty())
	{
		Result r;
		r.sendAction = lastSendAction_;
		r.content = content_;
		finished(r);
		return;
	}

	finishedConnection_ = filters_.front()->finished.connect(boost::bind(&MessageFilterStack::filterFinished, this, boost::placeholders::_1)),

	// may call filterFinished immediately
	filters_.front()->start(context_, content_);
}

void Filter::MessageFilterStack::filterFinished(const Result &result)
{
	if(!result.errorMessage.isNull())
	{
		filters_.clear();

		Result r;
		r.errorMessage = result.errorMessage;
		finished(r);
		return;
	}

	lastSendAction_ = result.sendAction;
	content_ = result.content;

	switch(lastSendAction_)
	{
		case Send:
			// remove the finished filter
			filters_.erase(filters_.begin());
			break;
		case Drop:
			// stop filtering. remove the finished filter and any remaining
			filters_.clear();
			break;
	}

	// will emit finished if there are no remaining filters
	nextFilter();
}
