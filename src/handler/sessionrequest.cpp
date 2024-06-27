/*
 * Copyright (C) 2016 Fanout, Inc.
 * Copyright (C) 2024 Fastly, Inc.
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

#include "sessionrequest.h"

#include <QVariant>
#include <QObject>
#include "qtcompat.h"
#include "zrpcmanager.h"
#include "zrpcrequest.h"
#include "deferred.h"
#include "detectrule.h"

namespace SessionRequest {

class DetectRulesSet : public Deferred
{
	Q_OBJECT

	Connection finishedConnection;

public:
	DetectRulesSet(ZrpcManager *stateClient, const QList<DetectRule> &rules, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		finishedConnection = req->finished.connect(boost::bind(&DetectRulesSet::req_finished, this, req));

		QVariantList rlist;
		foreach(const DetectRule &rule, rules)
		{
			QVariantHash i;
			i["domain"] = rule.domain.toUtf8();
			i["path-prefix"] = rule.pathPrefix;
			i["sid-ptr"] = rule.sidPtr.toUtf8();
			if(!rule.jsonParam.isEmpty())
				i["json-param"] = rule.jsonParam.toUtf8();
			rlist += i;
		}

		QVariantHash args;
		args["rules"] = rlist;
		req->start("session-detect-rules-set", args);
	}

private:
	void req_finished(ZrpcRequest *req)
	{
		if(req->success())
		{
			setFinished(true);
		}
		else
		{
			setFinished(false, req->errorCondition());
		}
	}
};

class DetectRulesGet : public Deferred
{
	Q_OBJECT

	Connection finishedConnection;

public:
	DetectRulesGet(ZrpcManager *stateClient, const QString &domain, const QByteArray &path, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		finishedConnection = req->finished.connect(boost::bind(&DetectRulesGet::req_finished, this, req));

		QVariantHash args;
		args["domain"] = domain.toUtf8();
		args["path"] = path;
		req->start("session-detect-rules-get", args);
	}

private:
	void req_finished(ZrpcRequest *req)
	{
		if(req->success())
		{
			QVariant vresult = req->result();
			if(typeId(vresult) != QMetaType::QVariantList)
			{
				setFinished(false);
				return;
			}

			QVariantList result = vresult.toList();

			QList<DetectRule> rules;
			foreach(const QVariant &vr, result)
			{
				if(typeId(vr) != QMetaType::QVariantHash)
				{
					setFinished(false);
					return;
				}

				QVariantHash r = vr.toHash();

				DetectRule rule;

				if(!r.contains("domain") || typeId(r["domain"]) != QMetaType::QByteArray)
				{
					setFinished(false);
					return;
				}

				rule.domain = QString::fromUtf8(r["domain"].toByteArray());

				if(!r.contains("path-prefix") || typeId(r["path-prefix"]) != QMetaType::QByteArray)
				{
					setFinished(false);
					return;
				}

				rule.pathPrefix = r["path-prefix"].toByteArray();

				if(!r.contains("sid-ptr") || typeId(r["sid-ptr"]) != QMetaType::QByteArray)
				{
					setFinished(false);
					return;
				}

				rule.sidPtr = QString::fromUtf8(r["sid-ptr"].toByteArray());

				if(r.contains("json-param"))
				{
					if(typeId(r["json-param"]) != QMetaType::QByteArray)
					{
						setFinished(false);
						return;
					}

					rule.jsonParam = QString::fromUtf8(r["json-param"].toByteArray());
				}

				rules += rule;
			}

			setFinished(true, QVariant::fromValue<DetectRuleList>(rules));
		}
		else
		{
			setFinished(false, req->errorCondition());
		}
	}
};

class CreateOrUpdate : public Deferred
{
	Q_OBJECT

	Connection finishedConnection;
	
public:
	CreateOrUpdate(ZrpcManager *stateClient, const QString &sid, const LastIds &lastIds, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		finishedConnection = req->finished.connect(boost::bind(&CreateOrUpdate::req_finished, this, req));

		QVariantHash args;

		args["sid"] = sid.toUtf8();

		QVariantHash vlastIds;
		QHashIterator<QString, QString> it(lastIds);
		while(it.hasNext())
		{
			it.next();
			vlastIds.insert(it.key(), it.value().toUtf8());
		}
		args["last-ids"] = vlastIds;

		req->start("session-create-or-update", args);
	}

private:
	void req_finished(ZrpcRequest *req)
	{
		if(req->success())
		{
			setFinished(true);
		}
		else
		{
			setFinished(false, req->errorCondition());
		}
	}
};

class UpdateMany : public Deferred
{
	Q_OBJECT

	Connection finishedConnection;
	
public:
	UpdateMany(ZrpcManager *stateClient, const QHash<QString, LastIds> &sidLastIds, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		finishedConnection = req->finished.connect(boost::bind(&UpdateMany::req_finished, this, req));

		QVariantHash vsidLastIds;

		QHashIterator<QString, LastIds> it(sidLastIds);
		while(it.hasNext())
		{
			it.next();
			const QString &sid = it.key();
			const LastIds &lastIds = it.value();

			QVariantHash vlastIds;

			QHashIterator<QString, QString> it(lastIds);
			while(it.hasNext())
			{
				it.next();
				vlastIds.insert(it.key(), it.value().toUtf8());
			}

			vsidLastIds.insert(sid, vlastIds);
		}

		QVariantHash args;
		args["sid-last-ids"] = vsidLastIds;
		req->start("session-update-many", args);
	}

private:
	void req_finished(ZrpcRequest *req)
	{
		if(req->success())
		{
			setFinished(true);
		}
		else
		{
			setFinished(false, req->errorCondition());
		}
	}
};

class GetLastIds : public Deferred
{
	Q_OBJECT

	Connection finishedConnection;
	
public:
	GetLastIds(ZrpcManager *stateClient, const QString &sid, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		finishedConnection = req->finished.connect(boost::bind(&GetLastIds::req_finished, this, req));

		QVariantHash args;
		args["sid"] = sid.toUtf8();
		req->start("session-get-last-ids", args);
	}

private:
	void req_finished(ZrpcRequest *req)
	{
		if(req->success())
		{
			QVariant vresult = req->result();
			if(typeId(vresult) != QMetaType::QVariantHash)
			{
				setFinished(false);
				return;
			}

			QVariantHash result = vresult.toHash();

			QHash<QString, QString> out;
			QHashIterator<QString, QVariant> it(result);
			while(it.hasNext())
			{
				it.next();
				const QVariant &i = it.value();
				if(typeId(i) != QMetaType::QByteArray)
				{
					setFinished(false);
					return;
				}

				out.insert(it.key(), QString::fromUtf8(i.toByteArray()));
			}

			setFinished(true, QVariant::fromValue<LastIds>(out));
		}
		else
		{
			setFinished(false, req->errorCondition());
		}
	}
};

Deferred *detectRulesSet(ZrpcManager *stateClient, const QList<DetectRule> &rules, QObject *parent)
{
	return new DetectRulesSet(stateClient, rules, parent);
}

Deferred *detectRulesGet(ZrpcManager *stateClient, const QString &domain, const QByteArray &path, QObject *parent)
{
	return new DetectRulesGet(stateClient, domain, path, parent);
}

Deferred *createOrUpdate(ZrpcManager *stateClient, const QString &sid, const LastIds &lastIds, QObject *parent)
{
	return new CreateOrUpdate(stateClient, sid, lastIds, parent);
}

Deferred *updateMany(ZrpcManager *stateClient, const QHash<QString, LastIds> &sidLastIds, QObject *parent)
{
	return new UpdateMany(stateClient, sidLastIds, parent);
}

Deferred *getLastIds(ZrpcManager *stateClient, const QString &sid, QObject *parent)
{
	return new GetLastIds(stateClient, sid, parent);
}

}

#include "sessionrequest.moc"
