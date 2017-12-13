/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "sessionrequest.h"

#include <QVariant>
#include <QObject>
#include "zrpcmanager.h"
#include "zrpcrequest.h"
#include "deferred.h"
#include "detectrule.h"

namespace SessionRequest {

class DetectRulesSet : public Deferred
{
	Q_OBJECT

public:
	DetectRulesSet(ZrpcManager *stateClient, const QList<DetectRule> &rules, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, &ZrpcRequest::finished, this, &DetectRulesSet::req_finished);

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

private slots:
	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

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

public:
	DetectRulesGet(ZrpcManager *stateClient, const QString &domain, const QByteArray &path, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, &ZrpcRequest::finished, this, &DetectRulesGet::req_finished);

		QVariantHash args;
		args["domain"] = domain.toUtf8();
		args["path"] = path;
		req->start("session-detect-rules-get", args);
	}

private slots:
	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

		if(req->success())
		{
			QVariant vresult = req->result();
			if(vresult.type() != QVariant::List)
			{
				setFinished(false);
				return;
			}

			QVariantList result = vresult.toList();

			QList<DetectRule> rules;
			foreach(const QVariant &vr, result)
			{
				if(vr.type() != QVariant::Hash)
				{
					setFinished(false);
					return;
				}

				QVariantHash r = vr.toHash();

				DetectRule rule;

				if(!r.contains("domain") || r["domain"].type() != QVariant::ByteArray)
				{
					setFinished(false);
					return;
				}

				rule.domain = QString::fromUtf8(r["domain"].toByteArray());

				if(!r.contains("path-prefix") || r["path-prefix"].type() != QVariant::ByteArray)
				{
					setFinished(false);
					return;
				}

				rule.pathPrefix = r["path-prefix"].toByteArray();

				if(!r.contains("sid-ptr") || r["sid-ptr"].type() != QVariant::ByteArray)
				{
					setFinished(false);
					return;
				}

				rule.sidPtr = QString::fromUtf8(r["sid-ptr"].toByteArray());

				if(r.contains("json-param"))
				{
					if(r["json-param"].type() != QVariant::ByteArray)
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

public:
	CreateOrUpdate(ZrpcManager *stateClient, const QString &sid, const LastIds &lastIds, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, &ZrpcRequest::finished, this, &CreateOrUpdate::req_finished);

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

private slots:
	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

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

public:
	UpdateMany(ZrpcManager *stateClient, const QHash<QString, LastIds> &sidLastIds, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, &ZrpcRequest::finished, this, &UpdateMany::req_finished);

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

private slots:
	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

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

public:
	GetLastIds(ZrpcManager *stateClient, const QString &sid, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, &ZrpcRequest::finished, this, &GetLastIds::req_finished);

		QVariantHash args;
		args["sid"] = sid.toUtf8();
		req->start("session-get-last-ids", args);
	}

private slots:
	void req_finished()
	{
		ZrpcRequest *req = (ZrpcRequest *)sender();

		if(req->success())
		{
			QVariant vresult = req->result();
			if(vresult.type() != QVariant::Hash)
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
				if(i.type() != QVariant::ByteArray)
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
