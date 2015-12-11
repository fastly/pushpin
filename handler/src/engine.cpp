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

#include "engine.h"

#include <assert.h>
#include <qjson/parser.h>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "log.h"
#include "packet/httprequestdata.h"
#include "packet/httpresponsedata.h"
#include "zutil.h"
#include "zrpcmanager.h"
#include "zrpcrequest.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"
#include "deferred.h"
#include "statusreasons.h"
#include "httpserver.h"

// TODO: set lingers

#define DEFAULT_HWM 1000
#define SUB_SNDHWM 0 // infinite

/*
CONNECTION_TTL = 600
CONNECTION_REFRESH = 540
CONNECTION_LINGER = 60

SUBSCRIPTION_TTL = 60
SUBSCRIPTION_LINGER = 60

# zmq socket linger
DEFAULT_LINGER = 1000

# delay to avoid overflowing the nic. wait 1ms for every 20 deliveries
SEND_BATCH_SIZE = 20
SEND_BATCH_DELAY = 0.001
*/

/*
class Hold(object):
	def __init__(self, rid, request, mode, response, auto_cross_origin, jsonp_callback):
		self.lock = threading.Lock()
		self.rid = rid
		self.out_seq = None # need to lock hold
		self.out_credits = 0 # need to lock hold
		self.request = request
		self.mode = mode
		self.response = response
		self.auto_cross_origin = auto_cross_origin
		self.jsonp_callback = jsonp_callback
		self.jsonp_extended_response = False
		self.expire_time = None
		self.last_keepalive = None
		self.grip_keep_alive = None
		self.grip_keep_alive_timeout = None
		self.last_send = None
		self.meta = {}
		self.channel_filters = {} # k=channel, v=filters

class WsSession(object):
	def __init__(self, cid):
		self.cid = cid
		self.expire_time = None
		self.channel_prefix = None
		self.sid = None
		self.meta = {}
		self.channel_filters = {} # k=channel, v=filters

class Subscription(object):
	def __init__(self, mode, channel):
		self.mode = mode
		self.channel = channel
		self.expire_time = None
		self.last_keepalive = None

class ConnectionInfo(object):
	def __init__(self, id, type):
		self.id = id
		self.type = type
		self.route = None
		self.peer_address = None
		self.ssl = False
		self.linger = False
		self.last_keepalive = None
		self.sid = None

class Session(object):
	def __init__(self):
		self.id = None

class StatsSubscription(object):
	def __init__(self):
		self.mode = None
		self.channel = None
		self.expire_time = None

lock = threading.Lock()
response_channels = dict()
response_lastids = dict()
stream_channels = dict()
channels_by_req = dict()
ws_sessions = dict()
ws_channels = dict()

# key=(type, channel)
subs = dict()

stats_lock = threading.Lock()
stats_activity = dict() # route, count
conns = dict() # conn id, ConnectionInfo

# assumes state is locked
# returns list of (mode, channel) that should be unsubscribed
def remove_from_req_channels(rid, response=True, stream=True):
	unsub = []
	req_channels = channels_by_req.get(rid)
	if req_channels is not None:
		to_remove = set()
		for mode, channel in req_channels:
			if response and mode == 'response':
				hchannels = response_channels.get(channel)
				if hchannels is not None and rid in hchannels:
					del hchannels[rid]
					if len(hchannels) == 0:
						del response_channels[channel]
						unsub.append((mode, channel))
				to_remove.add((mode, channel))
			elif stream and mode == 'stream':
				hchannels = stream_channels.get(channel)
				if hchannels is not None and rid in hchannels:
					del hchannels[rid]
					if len(hchannels) == 0:
						del stream_channels[channel]
						unsub.append((mode, channel))
				to_remove.add((mode, channel))
		for mode, channel in to_remove:
			req_channels.remove((mode, channel))
		if len(req_channels) == 0:
			del channels_by_req[rid]
	return unsub

# assumes state is locked
def remove_from_response_channels(rid):
	return remove_from_req_channels(rid, response=True, stream=False)

# assumes state is locked
def remove_from_stream_channels(rid):
	return remove_from_req_channels(rid, response=False, stream=True)

# return body size sent
def reply_http(sock, rid, code, reason, headers, body, nolen=False, seq=None):
	if isinstance(reason, unicode):
		reason = reason.encode("utf-8")

	# ensure headers are utf-8
	tmp = dict()
	for k, v in headers.iteritems():
		if isinstance(k, unicode):
				k = k.encode("utf-8")
		if isinstance(v, unicode):
				v = v.encode("utf-8")
		tmp[k] = v
	headers = tmp

	if isinstance(body, unicode):
		body = body.encode("utf-8")

	if nolen:
		header_remove(headers, "Content-Length")
	else:
		header_set(headers, "Content-Length", str(len(body)))

	out = dict()
	out["from"] = instance_id
	out["id"] = rid[1]
	if seq is not None:
		out["seq"] = seq
	out["code"] = code
	out["reason"] = reason
	headers_list = list()
	for k, v in headers.iteritems():
		headers_list.append([k, v])
	out["headers"] = headers_list
	if body:
		out["body"] = body
	if nolen:
		out["more"] = True

	m_raw = rid[0] + " T" + tnetstring.dumps(out)
	logger.debug("OUT publish: %s" % m_raw)
	sock.send(m_raw)
	return len(body)

def reply_http_chunk(sock, rid, content, seq=None):
	out = dict()
	out["from"] = instance_id
	out["id"] = rid[1]
	if seq is not None:
		out["seq"] = seq
	out["body"] = content
	out["more"] = True

	m_raw = rid[0] + " T" + tnetstring.dumps(out)
	logger.debug("OUT publish: %s" % m_raw)
	sock.send(m_raw)

def reply_http_close(sock, rid, seq=None):
	out = dict()
	out["from"] = instance_id
	out["id"] = rid[1]
	if seq is not None:
		out["seq"] = seq

	m_raw = rid[0] + " T" + tnetstring.dumps(out)
	logger.debug("OUT publish: %s" % m_raw)
	sock.send(m_raw)

simple_headers = set()
simple_headers.add("Cache-control")
simple_headers.add("Content-Language")
simple_headers.add("Content-Length")
simple_headers.add("Content-Type")
simple_headers.add("Expires")
simple_headers.add("Last-Modified")
simple_headers.add("Pragma")

# modifies response_headers as needed
def apply_cors_headers(request_headers, response_headers):
	if not header_get(response_headers, "Access-Control-Allow-Methods"):
		acr_method = header_get(request_headers, "Access-Control-Request-Method")
		if acr_method:
			header_set(response_headers, "Access-Control-Allow-Methods", acr_method)
		else:
			header_set(response_headers, "Access-Control-Allow-Methods", "OPTIONS, HEAD, GET, POST, PUT, DELETE")

	if not header_get(response_headers, "Access-Control-Allow-Headers"):
		acr_headers = header_get(request_headers, "Access-Control-Request-Headers")
		allow_headers = list()
		if acr_headers:
			for name in acr_headers.split(","):
				name = name.strip()
				if name:
					allow_headers.append(name)
		if len(allow_headers) > 0:
			header_set(response_headers, "Access-Control-Allow-Headers", ", ".join(allow_headers))

	if not header_get(response_headers, "Access-Control-Expose-Headers"):
		expose_headers = list()
		for name in response_headers.keys():
			lname = name.lower()
			if not header_names_contains(simple_headers, name) and not lname.startswith("access-control-") and not header_names_contains(expose_headers, name):
				expose_headers.append(name)
		if len(expose_headers) > 0:
			header_set(response_headers, "Access-Control-Expose-Headers", ", ".join(expose_headers))

	if not header_get(response_headers, "Access-Control-Allow-Credentials"):
		header_set(response_headers, "Access-Control-Allow-Credentials", "true")

	if not header_get(response_headers, "Access-Control-Allow-Origin"):
		origin = header_get(request_headers, "Origin")
		if not origin:
			origin = "*"
		header_set(response_headers, "Access-Control-Allow-Origin", origin)

	if not header_get(response_headers, "Access-Control-Max-Age"):
		header_set(response_headers, "Access-Control-Max-Age", "3600")

# return True to send and False to drop.
# TODO: support more than one filter, payload modification, etc
def apply_filters(sub_meta, pub_meta, filters):
	for f in filters:
		if f == 'skip-self':
			user = sub_meta.get('user')
			sender = pub_meta.get('sender')
			if user and sender and sender == user:
				return False
	return True
*/

class JsonPointer
{
public:
	QVariant *obj;
	QString childName;
	int childIndex;

	JsonPointer() :
		obj(0),
		childIndex(-2) // -2=unset, -1=end
	{
	}
};

// assumes data is qjson-style (hash->map, bytearray->string)
static JsonPointer resolveJsonPointer(QVariant &data, const QString &jsonPointerStr, QString *errorMessage = 0)
{
	if(!jsonPointerStr.startsWith('/'))
	{
		if(errorMessage)
			*errorMessage = "pointer must start with /";
		return JsonPointer();
	}

	JsonPointer ptr;
	ptr.obj = &data;

	// root
	if(jsonPointerStr.length() == 1)
		return ptr;

	QStringList parts = jsonPointerStr.split('/').mid(1);
	foreach(const QString &part, parts)
	{
		if(part.isEmpty())
		{
			if(errorMessage)
				*errorMessage = "reference cannot be empty";
			return JsonPointer();
		}

		QString p = part;
		p.replace("~1", "/");
		p.replace("~0", "~");

		if(!ptr.childName.isNull() || ptr.childIndex != -2)
		{
			if(errorMessage)
				*errorMessage = "cannot step into undefined reference";
			return JsonPointer();
		}

		if(ptr.obj->type() == QVariant::Map)
		{
			QVariantMap mobj = ptr.obj->toMap();

			if(mobj.contains(p))
				ptr.obj = &mobj[p];
			else
				ptr.childName = p;
		}
		else if(ptr.obj->type() == QVariant::List)
		{
			QVariantList lobj = ptr.obj->toList();

			if(p == "-")
			{
				ptr.childIndex = -1;
			}
			else
			{
				bool ok;
				int index = p.toInt(&ok);
				if(!ok)
				{
					if(errorMessage)
						*errorMessage = "index must be an integer";
					return JsonPointer();
				}

				if(index < 0 || index >= lobj.count())
				{
					if(errorMessage)
						*errorMessage = "index out of range";
					return JsonPointer();
				}

				ptr.obj = &lobj[index];
			}
		}
		else
		{
			if(errorMessage)
				*errorMessage = "non-container value cannot have child reference";
			return JsonPointer();
		}
	}

	return ptr;
}

// assumes data and ops are qjson-style (hash->map, bytearray->string)
static QVariant jsonPatch(const QVariant &data, const QVariantList &ops, QString *errorMessage = 0)
{
	QVariant out = data;

	foreach(const QVariant &vop, ops)
	{
		if(vop.type() != QVariant::Map)
		{
			if(errorMessage)
				*errorMessage = "invalid op";
			return QVariant();
		}

		QVariantMap op = vop.toMap();
		QByteArray type = op.value("op").toByteArray();
		if(type.isEmpty())
		{
			if(errorMessage)
				*errorMessage = "invalid op";
			return QVariant();
		}

		if(type == "add")
		{
			QString path = QString::fromUtf8(op.value("path").toByteArray());
			if(path.isEmpty())
			{
				if(errorMessage)
					*errorMessage = "invalid op";
				return QVariant();
			}

			if(!op.contains("value"))
			{
				if(errorMessage)
					*errorMessage = "invalid op";
				return QVariant();
			}

			// we assume this value is json compatible
			QVariant value = op["value"];

			QString msg;
			JsonPointer ptr = resolveJsonPointer(out, path, &msg);
			if(!ptr.obj)
			{
				if(errorMessage)
					*errorMessage = msg;
				return QVariant();
			}

			if(!ptr.childName.isNull())
			{
				ptr.obj->toMap()[ptr.childName] = value;
			}
			else if(ptr.childIndex != -2)
			{
				if(ptr.childIndex != -1)
					ptr.obj->toList().insert(ptr.childIndex, value);
				else
					ptr.obj->toList().append(value);
			}
			else
			{
				*(ptr.obj) = value;
			}
		}
		else
		{
			if(errorMessage)
				*errorMessage = QString("unsupported op: %1").arg(QString::fromUtf8(type));
			return QVariant();
		}
	}

	return out;
}

class DetectRule
{
public:
	QString domain;
	QByteArray pathPrefix;
	QString sidPtr;
	QString jsonParam;
};

typedef QList<DetectRule> DetectRuleList;
Q_DECLARE_METATYPE(DetectRuleList);

typedef QHash<QString, QString> LastIds;
Q_DECLARE_METATYPE(LastIds);

class SessionDetectRulesSet : public Deferred
{
	Q_OBJECT

public:
	SessionDetectRulesSet(ZrpcManager *stateClient, const QList<DetectRule> &rules, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, SIGNAL(finished()), SLOT(req_finished()));

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

class SessionDetectRulesGet : public Deferred
{
	Q_OBJECT

public:
	SessionDetectRulesGet(ZrpcManager *stateClient, const QString &domain, const QByteArray &path, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, SIGNAL(finished()), SLOT(req_finished()));

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

class SessionCreateOrUpdate : public Deferred
{
	Q_OBJECT

public:
	SessionCreateOrUpdate(ZrpcManager *stateClient, const QString &sid, const LastIds &lastIds, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, SIGNAL(finished()), SLOT(req_finished()));

		QVariantHash args;

		args["sid"] = sid.toUtf8();

		if(!lastIds.isEmpty())
		{
			QVariantHash vlastIds;
			QHashIterator<QString, QString> it(lastIds);
			while(it.hasNext())
			{
				it.next();
				vlastIds.insert(it.key(), it.value().toUtf8());
			}

			args["last-ids"] = vlastIds;
		}

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

class SessionUpdateMany : public Deferred
{
	Q_OBJECT

public:
	SessionUpdateMany(ZrpcManager *stateClient, const QHash<QString, LastIds> &sidLastIds, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, SIGNAL(finished()), SLOT(req_finished()));

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

class SessionGetLastIds : public Deferred
{
	Q_OBJECT

public:
	SessionGetLastIds(ZrpcManager *stateClient, const QString &sid, QObject *parent = 0) :
		Deferred(parent)
	{
		ZrpcRequest *req = new ZrpcRequest(stateClient, this);
		connect(req, SIGNAL(finished()), SLOT(req_finished()));

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

static void setSuccess(bool *ok, QString *errorMessage)
{
	if(ok)
		*ok = true;
	if(errorMessage)
		errorMessage->clear();
}

static void setError(bool *ok, QString *errorMessage, const QString &msg)
{
	if(ok)
		*ok = false;
	if(errorMessage)
		*errorMessage = msg;
}

static bool isKeyedObject(const QVariant &in)
{
	return (in.type() == QVariant::Hash || in.type() == QVariant::Map);
}

static QVariant createSameKeyedObject(const QVariant &in)
{
	if(in.type() == QVariant::Hash)
		return QVariantHash();
	else if(in.type() == QVariant::Map)
		return QVariantMap();
	else
		return QVariant();
}

static bool keyedObjectIsEmpty(const QVariant &in)
{
	if(in.type() == QVariant::Hash)
		return in.toHash().isEmpty();
	else if(in.type() == QVariant::Map)
		return in.toMap().isEmpty();
	else
		return true;
}

static bool keyedObjectContains(const QVariant &in, const QString &name)
{
	if(in.type() == QVariant::Hash)
		return in.toHash().contains(name);
	else if(in.type() == QVariant::Map)
		return in.toMap().contains(name);
	else
		return false;
}

static QVariant keyedObjectGetValue(const QVariant &in, const QString &name)
{
	if(in.type() == QVariant::Hash)
		return in.toHash().value(name);
	else if(in.type() == QVariant::Map)
		return in.toMap().value(name);
	else
		return QVariant();
}

static void keyedObjectInsert(QVariant *in, const QString &name, const QVariant &value)
{
	if(in->type() == QVariant::Hash)
	{
		QVariantHash h = in->toHash();
		h.insert(name, value);
		*in = h;
	}
	else if(in->type() == QVariant::Map)
	{
		QVariantMap h = in->toMap();
		h.insert(name, value);
		*in = h;
	}
}

static QVariant getChild(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok = 0, QString *errorMessage = 0)
{
	if(!isKeyedObject(in))
	{
		QString pn = !parentName.isEmpty() ? parentName : QString("value");
		setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
		return QVariant();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	QVariant v;
	if(in.type() == QVariant::Hash)
	{
		QVariantHash h = in.toHash();

		if(!h.contains(childName))
		{
			if(required)
				setError(ok, errorMessage, QString("%1 does not contain '%2'").arg(pn).arg(childName));
			else
				setSuccess(ok, errorMessage);

			return QVariant();
		}

		v = h[childName];
	}
	else // Map
	{
		QVariantMap m = in.toMap();

		if(!m.contains(childName))
		{
			if(required)
				setError(ok, errorMessage, QString("%1 does not contain '%2'").arg(pn).arg(childName));
			else
				setSuccess(ok, errorMessage);

			return QVariant();
		}

		v = m[childName];
	}

	setSuccess(ok, errorMessage);
	return v;
}

static QVariant getKeyedObject(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok = 0, QString *errorMessage = 0)
{
	bool ok_;
	QVariant v = getChild(in, parentName, childName, required, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return QVariant();
	}

	if(!v.isValid() && !required)
	{
		setSuccess(ok, errorMessage);
		return QVariant();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	if(!isKeyedObject(v))
	{
		setError(ok, errorMessage, QString("%1 contains '%2' with wrong type").arg(pn).arg(childName));
		return QVariant();
	}

	setSuccess(ok, errorMessage);
	return v;
}

static QVariantList getList(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok = 0, QString *errorMessage = 0)
{
	bool ok_;
	QVariant v = getChild(in, parentName, childName, required, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return QVariantList();
	}

	if(!v.isValid() && !required)
	{
		setSuccess(ok, errorMessage);
		return QVariantList();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	if(v.type() != QVariant::List)
	{
		setError(ok, errorMessage, QString("%1 contains '%2' with wrong type").arg(pn).arg(childName));
		return QVariantList();
	}

	setSuccess(ok, errorMessage);
	return v.toList();
}

static QString getString(const QVariant &in, bool *ok = 0)
{
	if(in.type() == QVariant::String)
	{
		if(ok)
			*ok = true;
		return in.toString();
	}
	else if(in.type() == QVariant::ByteArray)
	{
		if(ok)
			*ok = true;
		return QString::fromUtf8(in.toByteArray());
	}
	else
	{
		if(ok)
			*ok = false;
		return QString();
	}
}

static QString getString(const QVariant &in, const QString &parentName, const QString &childName, bool required, bool *ok = 0, QString *errorMessage = 0)
{
	bool ok_;
	QVariant v = getChild(in, parentName, childName, required, &ok_, errorMessage);
	if(!ok_)
	{
		if(ok)
			*ok = false;
		return QString();
	}

	if(!v.isValid() && !required)
	{
		setSuccess(ok, errorMessage);
		return QString();
	}

	QString pn = !parentName.isEmpty() ? parentName : QString("object");

	QString str = getString(v, &ok_);
	if(!ok_)
	{
		setError(ok, errorMessage, QString("%1 contains '%2' with wrong type").arg(pn).arg(childName));
		return QString();
	}

	setSuccess(ok, errorMessage);
	return str;
}

class Format
{
public:
	enum Type
	{
		HttpResponse,
		HttpStream,
		WebSocketMessage
	};

	Type type;
	int code;
	QByteArray reason;
	HttpHeaders headers;
	QByteArray body; // used for content with stream/ws
	bool close; // stream
	bool binary; // ws

	Format() :
		type((Type)-1),
		code(-1),
		close(false),
		binary(false)
	{
	}

	Format(Type _type) :
		type(_type),
		code(-1),
		close(false),
		binary(false)
	{
	}

	static Format fromVariant(Type type, const QVariant &in, bool *ok = 0, QString *errorMessage = 0)
	{
		QString pn;
		if(type == HttpResponse)
			pn = "'http-response'";
		else if(type == HttpStream)
			pn = "'http-stream'";
		else // WebSocketMessage
			pn = "'ws-message'";

		if(!isKeyedObject(in))
		{
			setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
			return Format();
		}

		Format out(type);
		bool ok_;

		if(type == HttpResponse)
		{
			if(keyedObjectContains(in, "code"))
			{
				QVariant vcode = keyedObjectGetValue(in, "code");
				if(!vcode.canConvert(QVariant::Int))
				{
					setError(ok, errorMessage, QString("%1 contains 'code' with wrong type").arg(pn));
					return Format();
				}

				out.code = vcode.toInt();

				if(out.code < 0 || out.code > 999)
				{
					setError(ok, errorMessage, QString("%1 contains 'code' with invalid value").arg(pn));
					return Format();
				}
			}
			else
				out.code = 200;

			QString reasonStr = getString(in, pn, "reason", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return Format();
			}

			if(!reasonStr.isEmpty())
				out.reason = reasonStr.toUtf8();
			else
				out.reason = StatusReasons::getReason(out.code);

			if(keyedObjectContains(in, "headers"))
			{
				QVariant vheaders = keyedObjectGetValue(in, "headers");
				if(vheaders.type() == QVariant::List)
				{
					foreach(const QVariant &vheader, vheaders.toList())
					{
						if(vheader.type() != QVariant::List)
						{
							setError(ok, errorMessage, "headers contains element with wrong type");
							return Format();
						}

						QVariantList lheader = vheader.toList();
						if(lheader.count() != 2)
						{
							setError(ok, errorMessage, "headers contains list with wrong number of elements");
							return Format();
						}

						QString name = getString(lheader[0], &ok_);
						if(!ok_)
						{
							setError(ok, errorMessage, "header contains name element with wrong type");
							return Format();
						}

						QString val = getString(lheader[1], &ok_);
						if(!ok_)
						{
							setError(ok, errorMessage, "header contains value element with wrong type");
							return Format();
						}

						out.headers += HttpHeader(name.toUtf8(), val.toUtf8());
					}
				}
				else if(isKeyedObject(vheaders))
				{
					if(vheaders.type() == QVariant::Hash)
					{
						QVariantHash hheaders = vheaders.toHash();

						QHashIterator<QString, QVariant> it(hheaders);
						while(it.hasNext())
						{
							it.next();
							const QString &key = it.key();
							const QVariant &vval = it.value();

							QString val = getString(vval, &ok_);
							if(!ok_)
							{
								setError(ok, errorMessage, QString("headers contains '%1' with wrong type").arg(key));
								return Format();
							}

							out.headers += HttpHeader(key.toUtf8(), val.toUtf8());
						}
					}
					else // Map
					{
						QVariantMap mheaders = vheaders.toMap();

						QMapIterator<QString, QVariant> it(mheaders);
						while(it.hasNext())
						{
							it.next();
							const QString &key = it.key();
							const QVariant &vval = it.value();

							QString val = getString(vval, &ok_);
							if(!ok_)
							{
								setError(ok, errorMessage, QString("headers contains '%1' with wrong type").arg(key));
								return Format();
							}

							out.headers += HttpHeader(key.toUtf8(), val.toUtf8());
						}
					}
				}
				else
				{
					setError(ok, errorMessage, QString("%1 contains 'headers' with wrong type").arg(pn));
					return Format();
				}
			}

			if(in.type() == QVariant::Map && keyedObjectContains(in, "body-bin")) // JSON input
			{
				QString bodyBin = getString(in, pn, "body-bin", false, &ok_, errorMessage);
				if(!ok_)
				{
					if(ok)
						*ok = false;
					return Format();
				}

				out.body = QByteArray::fromBase64(bodyBin.toUtf8());
			}
			else if(keyedObjectContains(in, "body"))
			{
				QVariant vcontent = keyedObjectGetValue(in, "body");
				if(vcontent.type() == QVariant::ByteArray)
					out.body = vcontent.toByteArray();
				else if(vcontent.type() == QVariant::String)
					out.body = vcontent.toString().toUtf8();
				else
				{
					setError(ok, errorMessage, QString("%1 contains 'body' with wrong type").arg(pn));
					return Format();
				}
			}
			else
			{
				if(in.type() == QVariant::Map) // JSON input
					setError(ok, errorMessage, QString("%1 does not contain 'body' or 'body-bin'").arg(pn));
				else
					setError(ok, errorMessage, QString("%1 does not contain 'body'").arg(pn));
				return Format();
			}
		}
		else if(type == HttpStream)
		{
			QString action = getString(in, pn, "action", false, &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return Format();
			}

			if(action == "close")
				out.close = true;

			if(!out.close)
			{
				if(in.type() == QVariant::Map && keyedObjectContains(in, "content-bin")) // JSON input
				{
					QString contentBin = getString(in, pn, "content-bin", false, &ok_, errorMessage);
					if(!ok_)
					{
						if(ok)
							*ok = false;
						return Format();
					}

					out.body = QByteArray::fromBase64(contentBin.toUtf8());
				}
				else if(keyedObjectContains(in, "content"))
				{
					QVariant vcontent = keyedObjectGetValue(in, "content");
					if(vcontent.type() == QVariant::ByteArray)
						out.body = vcontent.toByteArray();
					else if(vcontent.type() == QVariant::String)
						out.body = vcontent.toString().toUtf8();
					else
					{
						setError(ok, errorMessage, QString("%1 contains 'content' with wrong type").arg(pn));
						return Format();
					}
				}
				else
				{
					if(in.type() == QVariant::Map) // JSON input
						setError(ok, errorMessage, QString("%1 does not contain 'content' or 'content-bin'").arg(pn));
					else
						setError(ok, errorMessage, QString("%1 does not contain 'content'").arg(pn));
					return Format();
				}
			}
		}
		else if(type == WebSocketMessage)
		{
			if(keyedObjectContains(in, "content-bin"))
			{
				QVariant vcontentBin = keyedObjectGetValue(in, "content-bin");

				if(in.type() == QVariant::Map) // JSON input
				{
					if(vcontentBin.type() != QVariant::String)
					{
						setError(ok, errorMessage, QString("%1 contains 'content-bin' with wrong type").arg(pn));
						return Format();
					}

					out.body = QByteArray::fromBase64(vcontentBin.toString().toUtf8());
				}
				else
				{
					if(vcontentBin.type() != QVariant::ByteArray)
					{
						setError(ok, errorMessage, QString("%1 contains 'content-bin' with wrong type").arg(pn));
						return Format();
					}

					out.body = vcontentBin.toByteArray();
				}

				out.binary = true;
			}
			else if(keyedObjectContains(in, "content"))
			{
				QVariant vcontent = keyedObjectGetValue(in, "content");
				if(vcontent.type() == QVariant::ByteArray)
					out.body = vcontent.toByteArray();
				else if(vcontent.type() == QVariant::String)
					out.body = vcontent.toString().toUtf8();
				else
				{
					setError(ok, errorMessage, QString("%1 contains 'content' with wrong type").arg(pn));
					return Format();
				}
			}
			else
			{
				setError(ok, errorMessage, QString("%1 does not contain 'content' or 'content-bin'").arg(pn));
				return Format();
			}
		}

		if(ok)
			*ok = true;
		return out;
	}
};

class PublishItem
{
public:
	QString channel;
	QString id;
	QString prevId;
	QHash<Format::Type, Format> formats;
	QHash<QString, QString> meta;

	static PublishItem fromVariant(const QVariant &vitem, bool *ok = 0, QString *errorMessage = 0)
	{
		QString pn = "publish item object";

		if(!isKeyedObject(vitem))
		{
			setError(ok, errorMessage, QString("%1 is not an object").arg(pn));
			return PublishItem();
		}

		PublishItem item;

		bool ok_;
		item.channel = getString(vitem, pn, "channel", true, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishItem();
		}

		item.id = getString(vitem, pn, "id", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishItem();
		}

		item.prevId = getString(vitem, pn, "prev-id", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishItem();
		}

		QVariant vformats = getKeyedObject(vitem, pn, "formats", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishItem();
		}

		if(!vformats.isValid())
		{
			vformats = createSameKeyedObject(vitem);

			QVariant v = keyedObjectGetValue(vitem, "http-response");
			if(v.isValid())
				keyedObjectInsert(&vformats, "http-response", v);

			v = keyedObjectGetValue(vitem, "http-stream");
			if(v.isValid())
				keyedObjectInsert(&vformats, "http-stream", v);

			v = keyedObjectGetValue(vitem, "ws-message");
			if(v.isValid())
				keyedObjectInsert(&vformats, "ws-message", v);
		}

		if(keyedObjectIsEmpty(vformats))
		{
			setError(ok, errorMessage, "no formats specified");
			return PublishItem();
		}

		if(keyedObjectContains(vformats, "http-response"))
		{
			Format f = Format::fromVariant(Format::HttpResponse, keyedObjectGetValue(vformats, "http-response"), &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return PublishItem();
			}

			item.formats.insert(Format::HttpResponse, f);
		}

		if(keyedObjectContains(vformats, "http-stream"))
		{
			Format f = Format::fromVariant(Format::HttpStream, keyedObjectGetValue(vformats, "http-stream"), &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return PublishItem();
			}

			item.formats.insert(Format::HttpStream, f);
		}

		if(keyedObjectContains(vformats, "ws-message"))
		{
			Format f = Format::fromVariant(Format::WebSocketMessage, keyedObjectGetValue(vformats, "ws-message"), &ok_, errorMessage);
			if(!ok_)
			{
				if(ok)
					*ok = false;
				return PublishItem();
			}

			item.formats.insert(Format::WebSocketMessage, f);
		}

		QVariant vmeta = getKeyedObject(vitem, pn, "meta", false, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return PublishItem();
		}

		if(vmeta.isValid())
		{
			if(vmeta.type() == QVariant::Hash)
			{
				QVariantHash hmeta = vmeta.toHash();

				QHashIterator<QString, QVariant> it(hmeta);
				while(it.hasNext())
				{
					it.next();
					const QString &key = it.key();
					const QVariant &vval = it.value();

					QString val = getString(vval, &ok_);
					if(!ok_)
					{
						setError(ok, errorMessage, QString("'meta' contains '%1' with wrong type").arg(key));
						return PublishItem();
					}

					item.meta[key] = val;
				}
			}
			else // Map
			{
				QVariantMap mmeta = vmeta.toMap();

				QMapIterator<QString, QVariant> it(mmeta);
				while(it.hasNext())
				{
					it.next();
					const QString &key = it.key();
					const QVariant &vval = it.value();

					QString val = getString(vval, &ok_);
					if(!ok_)
					{
						setError(ok, errorMessage, QString("'meta' contains '%1' with wrong type").arg(key));
						return PublishItem();
					}

					item.meta[key] = val;
				}
			}
		}

		setSuccess(ok, errorMessage);
		return item;
	}
};

static QList<PublishItem> parseHttpItems(const QVariantList &vitems, bool *ok = 0, QString *errorMessage = 0)
{
	QList<PublishItem> out;

	foreach(const QVariant &vitem, vitems)
	{
		bool ok_;
		PublishItem item = PublishItem::fromVariant(vitem, &ok_, errorMessage);
		if(!ok_)
		{
			if(ok)
				*ok = false;
			return QList<PublishItem>();
		}

		out += item;
	}

	setSuccess(ok, errorMessage);
	return out;
}

enum HoldMode
{
	NoHold,
	ResponseHold,
	StreamHold
};

class Hold
{
public:
	HoldMode mode;
	QSet<QString> channels;
	HttpResponseData response;
	ZhttpRequest *req;
};

class CommonState
{
public:
	QList<Hold*> holds;
};

typedef QPair<QByteArray, QByteArray> ByteArrayPair;

class InspectWorker : public Deferred
{
	Q_OBJECT

public:
	ZrpcRequest *req;
	ZrpcManager *stateClient;
	bool shareAll;
	HttpRequestData requestData;
	bool truncated;
	QString sid;
	LastIds lastIds;

	InspectWorker(ZrpcRequest *_req, ZrpcManager *_stateClient, bool _shareAll, QObject *parent = 0) :
		Deferred(parent),
		req(_req),
		stateClient(_stateClient),
		shareAll(_shareAll),
		truncated(false)
	{
		req->setParent(this);

		if(req->method() == "inspect")
		{
			QVariantHash args = req->args();

			if(!args.contains("method") || args["method"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.method = QString::fromLatin1(args["method"].toByteArray());

			if(!args.contains("uri") || args["uri"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.uri = QUrl(args["uri"].toString(), QUrl::StrictMode);
			if(!requestData.uri.isValid())
			{
				respondError("bad-request");
				return;
			}

			if(!args.contains("headers") || args["headers"].type() != QVariant::List)
			{
				respondError("bad-request");
				return;
			}

			foreach(const QVariant &vheader, args["headers"].toList())
			{
				if(vheader.type() != QVariant::List)
				{
					respondError("bad-request");
					return;
				}

				QVariantList vlist = vheader.toList();
				if(vlist.count() != 2 || vlist[0].type() != QVariant::ByteArray || vlist[1].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				requestData.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
			}

			if(!args.contains("body") || args["body"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.body = args["body"].toByteArray();

			truncated = false;
			if(args.contains("truncated"))
			{
				if(args["truncated"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				truncated = args["truncated"].toBool();
			}

			bool getSession = false;
			if(args.contains("get-session"))
			{
				if(args["get-session"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				getSession = args["get-session"].toBool();
			}

			if(getSession && stateClient)
			{
				// determine session info
				Deferred *d = new SessionDetectRulesGet(stateClient, requestData.uri.host().toUtf8(), requestData.uri.encodedPath(), this);
				connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionDetectRulesGet_finished(const DeferredResult &)));
				return;
			}

			doFinish();
		}
		else
		{
			respondError("method-not-found");
		}
	}

private:
	void respondError(const QByteArray &condition)
	{
		req->respondError(condition);
		setFinished(true);
	}

	void doFinish()
	{
		QVariantHash result;
		result["no-proxy"] = false;

		if(shareAll)
			result["sharing-key"] = requestData.method.toLatin1() + '|' + requestData.uri.toEncoded();

		if(!sid.isEmpty())
		{
			result["sid"] = sid.toUtf8();

			if(!lastIds.isEmpty())
			{
				QVariantHash vlastIds;
				QHashIterator<QString, QString> it(lastIds);
				while(it.hasNext())
				{
					it.next();
					vlastIds.insert(it.key(), it.value().toUtf8());
				}

				result["last-ids"] = vlastIds;
			}
		}

		req->respond(result);
		setFinished(true);
	}

private slots:
	void sessionDetectRulesGet_finished(const DeferredResult &result)
	{
		if(result.success)
		{
			QList<DetectRule> rules = result.value.value<DetectRuleList>();
			log_debug("retrieved %d rules", rules.count());

			foreach(const DetectRule &rule, rules)
			{
				QByteArray jsonData;

				if(!rule.jsonParam.isEmpty())
				{
					// hack to use QUrl's query string parser on form data
					QUrl tmp;
					tmp.setEncodedQuery(requestData.body);
					jsonData = tmp.queryItemValue(rule.jsonParam).toUtf8();
				}
				else
				{
					jsonData = requestData.body;
				}

				QJson::Parser parser;
				bool ok;
				QVariant vdata = parser.parse(jsonData, &ok);
				if(!ok)
					continue;

				JsonPointer ptr = resolveJsonPointer(vdata, rule.sidPtr);
				if(ptr.obj && ptr.childName.isNull() && ptr.childIndex == -2)
				{
					sid = ptr.obj->toString();
					break;
				}
			}

			if(!sid.isEmpty())
			{
				Deferred *d = new SessionGetLastIds(stateClient, sid, this);
				connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionGetLastIds_finished(const DeferredResult &)));
				return;
			}
		}
		else
		{
			// log error but keep going
			log_error("failed to detect session");
		}

		doFinish();
	}

	void sessionGetLastIds_finished(const DeferredResult &result)
	{
		if(result.success)
		{
			lastIds = result.value.value<LastIds>();
		}
		else
		{
			QByteArray errorCondition = result.value.toByteArray();

			if(errorCondition != "item-not-found")
			{
				// log error but keep going
				log_error("failed to detect session");
			}
		}

		doFinish();
	}
};

class RequestState
{
public:
	QPair<QByteArray, QByteArray> rid;
	int inSeq;
	int outSeq;

	RequestState() :
		inSeq(0),
		outSeq(0)
	{
	}
};

class Instruct
{
public:
	class Channel
	{
	public:
		QString name;
		QString prevId;
		QStringList filters;
	};

	HoldMode holdMode;
	QList<Channel> channels;
	int timeout;
	QList<QByteArray> exposeHeaders;
	QByteArray keepAlive;
	int keepAliveTimeout;
	QHash<QString, QString> meta;
	HttpResponseData response;

	Instruct() :
		holdMode(NoHold),
		timeout(-1),
		keepAliveTimeout(-1)
	{
	}

	static Instruct fromResponse(const HttpResponseData &response, bool *ok = 0)
	{
		HoldMode holdMode = NoHold;
		QList<Channel> channels;
		int timeout = -1;
		QList<QByteArray> exposeHeaders;
		QByteArray keepAlive;
		int keepAliveTimeout = -1;
		QHash<QString, QString> meta;
		HttpResponseData newResponse;

		if(response.headers.contains("Grip-Hold"))
		{
			QByteArray gripHoldStr = response.headers.get("Grip-Hold");
			if(gripHoldStr == "response")
			{
				holdMode = ResponseHold;
			}
			else if(gripHoldStr == "stream")
			{
				holdMode = StreamHold;
			}
			else
			{
				if(ok)
					*ok = false;
				return Instruct();
			}
		}

		QList<HttpHeaderParameters> gripChannels = response.headers.getAllAsParameters("Grip-Channel");
		foreach(const HttpHeaderParameters &gripChannel, gripChannels)
		{
			if(gripChannel.isEmpty())
			{
				if(ok)
					*ok = false;
				return Instruct();
			}

			Channel c;
			c.name = gripChannel[0].first;
			c.prevId = gripChannel.get("prev-id");

			for(int n = 1; n < gripChannel.count(); ++n)
			{
				const HttpHeaderParameter &param = gripChannel[n];
				if(param.first == "filter")
					c.filters += QString::fromUtf8(param.second);
			}

			channels += c;
		}

		if(response.headers.contains("Grip-Timeout"))
		{
			bool x;
			timeout = response.headers.get("Grip-Timeout").toInt(&x);
			if(!x)
			{
				if(ok)
					*ok = false;
				return Instruct();
			}
		}

		exposeHeaders = response.headers.getAll("Grip-Expose-Headers");

		HttpHeaderParameters keepAliveParams = response.headers.getAsParameters("Grip-Keep-Alive");
		if(!keepAliveParams.isEmpty())
		{
			QByteArray val = keepAliveParams[0].first;
			if(val.isEmpty())
			{
				if(ok)
					*ok = false;
				return Instruct();
			}

			if(keepAliveParams.contains("timeout"))
			{
				bool x;
				keepAliveTimeout = keepAliveParams.get("timeout").toInt(&x);
				if(!x)
				{
					if(ok)
						*ok = false;
					return Instruct();
				}
			}
			else
			{
				keepAliveTimeout = 55;
			}

			QByteArray format = keepAliveParams.get("format");
			if(format.isEmpty() || format == "raw")
			{
				keepAlive = val;
			}
			else if(format == "cstring")
			{
				// TODO: keepAlive = unescape(val)
			}
			else if(format == "base64")
			{
				keepAlive = QByteArray::fromBase64(val);
			}
			else
			{
				// raise ValueError('invalid keep alive format')
				if(ok)
					*ok = false;
				return Instruct();
			}

			if(keepAliveTimeout < 1)
			{
				keepAlive.clear();
				keepAliveTimeout = -1;
			}
		}

		QList<HttpHeaderParameters> metaParams = response.headers.getAllAsParameters("Grip-Set-Meta", HttpHeaders::ParseAllParameters);
		foreach(const HttpHeaderParameters &metaParam, metaParams)
		{
			if(metaParam.isEmpty())
			{
				if(ok)
					*ok = false;
				return Instruct();
			}

			meta[QString::fromUtf8(metaParam[0].first)] = QString::fromUtf8(metaParam[0].second);
		}

		QByteArray contentType = response.headers.getAsFirstParameter("Content-Type");
		if(contentType == "application/grip-instruct")
		{
			// TODO: process body
			/*if m['response']['code'] != 200:
				raise ValueError('response code for grip-instruct must be 200')
			instruct = json.loads(m['response']['body'])
			hold = instruct['hold']
			mode = hold.get('mode')
			if mode is None:
				mode = 'response'
			for hc in hold['channels']:
				name = channel_prefix + hc['name']
				prev_id = hc.get('prev-id')
				filters = hc.get('filters', [])
				if not isinstance(filters, list):
					raise ValueError('filters on channel must be a list')
				channels.append((name, prev_id, filters))
			if 'timeout' in hold:
				timeout = int(hold['timeout'])
			if 'keep-alive' in hold:
				ka = hold['keep-alive']
				if 'content-bin' in ka:
					keep_alive = b64decode(ka['content-bin'])
				else:
					keep_alive = ka['content'].encode('utf-8')
				keep_alive_timeout = int(ka['timeout'])
			if 'meta' in hold:
				meta = hold['meta']
			response = instruct.get('response')
			if response is None:
				response = dict()
				response['body'] = ''
			if "headers" in response and isinstance(response["headers"], list):
				d = dict()
				for i in response["headers"]:
					d[i[0]] = i[1]
				response["headers"] = d
			if "body-bin" in response:
				response["body"] = b64decode(response["body-bin"])
				del response["body-bin"]
			elif "body" in response:
				response["body"] = response["body"].encode("utf-8")
			else:
				response["body"] = ""
			*/
		}
		else
		{
			newResponse = response;

			newResponse.headers.clear();
			foreach(const HttpHeader &h, response.headers)
			{
				// strip out grip headers
				if(qstrnicmp(h.first.data(), "Grip-", 5) == 0)
					continue;

				if(!exposeHeaders.isEmpty())
				{
					QByteArray hlower = h.first.toLower();
					bool found = false;
					foreach(const QByteArray &e, exposeHeaders)
					{
						if(e.toLower() == hlower)
						{
							found = true;
							break;
						}
					}

					if(!found)
						continue;
				}

				newResponse.headers += HttpHeader(h.first, h.second);
			}
		}

		Instruct i;
		i.holdMode = holdMode;
		i.channels = channels;
		i.timeout = timeout;
		i.exposeHeaders = exposeHeaders;
		i.keepAlive = keepAlive;
		i.keepAliveTimeout = keepAliveTimeout;
		i.meta = meta;
		i.response = newResponse;

		if(ok)
			*ok = true;
		return i;
	}
};

class AcceptWorker : public Deferred
{
	Q_OBJECT

public:
	ZrpcRequest *req;
	ZrpcManager *stateClient;
	CommonState *cs;
	ZhttpManager *zhttpIn;
	QString route;
	QString channelPrefix;
	QHash<ByteArrayPair, RequestState> requestStates;
	HttpRequestData requestData;
	HttpResponseData responseData;
	QString sid;
	LastIds lastIds;

	AcceptWorker(ZrpcRequest *_req, ZrpcManager *_stateClient, CommonState *_cs, ZhttpManager *_zhttpIn, QObject *parent = 0) :
		Deferred(parent),
		req(_req),
		stateClient(_stateClient),
		cs(_cs),
		zhttpIn(_zhttpIn)
	{
		req->setParent(this);

		/*out_sock = ctx.socket(zmq.PUB)
		out_sock.linger = DEFAULT_LINGER
		for spec in m2a_out_specs:
			out_sock.connect(spec)

		retry_sock = ctx.socket(zmq.PUSH)
		retry_sock.linger = 0
		retry_sock.connect(proxy_retry_out_spec)

		stats_sock = ctx.socket(zmq.PUSH)
		stats_sock.linger = 0
		stats_sock.connect('inproc://stats_in')*/

		if(req->method() == "accept")
		{
			QVariantHash args = req->args();

			if(args.contains("route"))
			{
				if(args["route"].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				route = QString::fromUtf8(args["route"].toByteArray());
			}

			if(args.contains("channel-prefix"))
			{
				if(args["channel-prefix"].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				channelPrefix = QString::fromUtf8(args["channel-prefix"].toByteArray());
			}

			// parse requests

			if(!args.contains("requests") || args["requests"].type() != QVariant::List)
			{
				respondError("bad-request");
				return;
			}

			foreach(const QVariant &vr, args["requests"].toList())
			{
				if(vr.type() != QVariant::Hash)
				{
					respondError("bad-request");
					return;
				}

				QVariantHash r = vr.toHash();
				RequestState rs;

				if(!r.contains("rid") || r["rid"].type() != QVariant::Hash)
				{
					respondError("bad-request");
					return;
				}

				QVariantHash vrid = r["rid"].toHash();

				if(!vrid.contains("sender") || vrid["sender"].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				if(!vrid.contains("id") || vrid["id"].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				rs.rid = QPair<QByteArray, QByteArray>(vrid["sender"].toByteArray(), vrid["id"].toByteArray());

				if(!r.contains("in-seq") || !r["in-seq"].canConvert(QVariant::Int))
				{
					respondError("bad-request");
					return;
				}

				rs.inSeq = r["in-seq"].toInt();

				if(!r.contains("out-seq") || !r["out-seq"].canConvert(QVariant::Int))
				{
					respondError("bad-request");
					return;
				}

				rs.outSeq = r["out-seq"].toInt();

				requestStates.insert(rs.rid, rs);
			}

			// parse request-data

			if(!args.contains("request-data") || args["request-data"].type() != QVariant::Hash)
			{
				respondError("bad-request");
				return;
			}

			QVariantHash rd = args["request-data"].toHash();

			if(!rd.contains("method") || rd["method"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.method = QString::fromLatin1(rd["method"].toByteArray());

			if(!rd.contains("uri") || rd["uri"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.uri = QUrl(rd["uri"].toString(), QUrl::StrictMode);
			if(!requestData.uri.isValid())
			{
				respondError("bad-request");
				return;
			}

			if(!rd.contains("headers") || rd["headers"].type() != QVariant::List)
			{
				respondError("bad-request");
				return;
			}

			foreach(const QVariant &vheader, rd["headers"].toList())
			{
				if(vheader.type() != QVariant::List)
				{
					respondError("bad-request");
					return;
				}

				QVariantList vlist = vheader.toList();
				if(vlist.count() != 2 || vlist[0].type() != QVariant::ByteArray || vlist[1].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				requestData.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
			}

			if(!rd.contains("body") || rd["body"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			requestData.body = rd["body"].toByteArray();

			// parse response

			if(!args.contains("response") || args["response"].type() != QVariant::Hash)
			{
				respondError("bad-request");
				return;
			}

			rd = args["response"].toHash();

			if(!rd.contains("code") || !rd["code"].canConvert(QVariant::Int))
			{
				respondError("bad-request");
				return;
			}

			responseData.code = rd["code"].toInt();

			if(!rd.contains("reason") || rd["reason"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			responseData.reason = rd["reason"].toByteArray();

			if(!rd.contains("headers") || rd["headers"].type() != QVariant::List)
			{
				respondError("bad-request");
				return;
			}

			foreach(const QVariant &vheader, rd["headers"].toList())
			{
				if(vheader.type() != QVariant::List)
				{
					respondError("bad-request");
					return;
				}

				QVariantList vlist = vheader.toList();
				if(vlist.count() != 2 || vlist[0].type() != QVariant::ByteArray || vlist[1].type() != QVariant::ByteArray)
				{
					respondError("bad-request");
					return;
				}

				responseData.headers += HttpHeader(vlist[0].toByteArray(), vlist[1].toByteArray());
			}

			if(!rd.contains("body") || rd["body"].type() != QVariant::ByteArray)
			{
				respondError("bad-request");
				return;
			}

			responseData.body = rd["body"].toByteArray();

			bool useSession = false;
			if(args.contains("use-session"))
			{
				if(args["use-session"].type() != QVariant::Bool)
				{
					respondError("bad-request");
					return;
				}

				useSession = args["use-session"].toBool();
			}

			sid = QString::fromUtf8(responseData.headers.get("Grip-Session-Id"));

			QList<DetectRule> rules;
			QList<HttpHeaderParameters> ruleHeaders = responseData.headers.getAllAsParameters("Grip-Session-Detect", HttpHeaders::ParseAllParameters);
			foreach(const HttpHeaderParameters &params, ruleHeaders)
			{
				if(params.contains("path-prefix") && params.contains("sid-ptr"))
				{
					DetectRule rule;
					rule.domain = requestData.uri.host();
					rule.pathPrefix = params.get("path-prefix");
					rule.sidPtr = QString::fromUtf8(params.get("sid-ptr"));
					if(params.contains("json-param"))
						rule.jsonParam = QString::fromUtf8(params.get("json-param"));
					rules += rule;
				}
			}

			QList<HttpHeaderParameters> lastHeaders = responseData.headers.getAllAsParameters("Grip-Last");
			foreach(const HttpHeaderParameters &params, lastHeaders)
			{
				lastIds.insert(params[0].first, params.get("last-id"));
			}

			if(useSession && stateClient)
			{
				if(!rules.isEmpty())
				{
					Deferred *d = new SessionDetectRulesSet(stateClient, rules, this);
					connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionDetectRulesSet_finished(const DeferredResult &)));
				}
				else
				{
					afterSetRules();
				}

				return;
			}

			afterSessionCalls();
		}
		else
		{
			respondError("method-not-found");
		}
	}

private:
	void respondError(const QByteArray &condition)
	{
		req->respondError(condition);
		setFinished(true);
	}

	void afterSetRules()
	{
		if(!sid.isEmpty())
		{
			Deferred *d = new SessionCreateOrUpdate(stateClient, sid, lastIds, this);
			connect(d, SIGNAL(finished(const DeferredResult &)), SLOT(sessionCreateOrUpdate_finished(const DeferredResult &)));
		}
		else
		{
			afterSessionCalls();
		}
	}

	void afterSessionCalls()
	{
		// TODO: support non-hold grip-instruct

		QByteArray contentType = responseData.headers.getAsFirstParameter("Content-Type");
		if(contentType != "application/grip-instruct" && !responseData.headers.contains("Grip-Hold"))
		{
			QVariantHash vresponse;
			vresponse["code"] = responseData.code;
			vresponse["reason"] = responseData.reason;
			QVariantList vheaders;
			foreach(const HttpHeader &h, responseData.headers)
			{
				// strip out grip headers
				if(qstrnicmp(h.first.data(), "Grip-", 5) == 0)
					continue;

				QVariantList vheader;
				vheader += h.first;
				vheader += h.second;
				vheaders += QVariant(vheader);
			}
			vresponse["headers"] = vheaders;
			vresponse["body"] = responseData.body;

			QVariantHash result;
			result["response"] = vresponse;
			req->respond(result);

			setFinished(true);
			return;
		}

		QVariantHash result;
		result["accepted"] = true;
		req->respond(result);

		bool ok;
		Instruct instruct = Instruct::fromResponse(responseData, &ok);
		if(!ok || instruct.holdMode == NoHold)
		{
			log_debug("failed to parse accept instructions");
			/*for req in reqs:
				rid = (req["rid"]["sender"], req["rid"]["id"])
				rheaders = dict()
				rheaders['Content-Type'] = 'text/plain'
				reply_http(out_sock, rid, 502, 'Bad Gateway', rheaders, 'Error while proxying to origin.\n')
			continue*/
			setFinished(true);
			return;
		}

		log_debug("accepting %d requests", requestStates.count());

		foreach(const RequestState &rs, requestStates)
		{
			// TODO
			/*rid = (req["rid"]["sender"], req["rid"]["id"])

			stats_lock.acquire()
			ci = ConnectionInfo("%s:%s" % (rid[0], rid[1]), "http")
			ci.route = route
			if "peer-address" in req:
				ci.peer_address = req["peer-address"]
			if "https" in req:
				ci.ssl = req["https"]
			ci.last_keepalive = int(time.time())
			ci.sid = sid
			# note: if we had a lingering connection, this will replace it
			conns[ci.id] = ci
			stats_lock.release()

			h = Hold(rid, m["request-data"], mode, response, req.get("auto-cross-origin"), req.get("jsonp-callback"))
			now = int(time.time())
			h.last_keepalive = now
			h.out_seq = req["out-seq"]
			h.out_credits = req["out-credits"]
			h.jsonp_extended_response = req.get("jsonp-extended-response", False)
			h.grip_keep_alive = keep_alive
			h.grip_keep_alive_timeout = keep_alive_timeout
			h.last_send = now
			h.meta = meta

			notify_subs = set()*/
			if(instruct.holdMode == ResponseHold)
			{
				// TODO
				ZhttpRequest::ServerState ss;
				ss.rid = ZhttpRequest::Rid(rs.rid.first, rs.rid.second);
				ss.inSeq = rs.inSeq;
				ss.outSeq = rs.outSeq;
				ss.outCredits = 200000;

				ZhttpRequest *outReq = zhttpIn->createRequestFromState(ss);

				Hold *hold = new Hold;
				hold->mode = instruct.holdMode;
				hold->response = instruct.response;
				hold->req = outReq;
				foreach(const Instruct::Channel &c, instruct.channels)
				{
					log_debug("adding response hold on %s", qPrintable(c.name));
					hold->channels += c.name;
				}
				cs->holds += hold;

				/*# bind channels
				quit = False
				lock.acquire()
				for channel, prev_id, filters in channels:
					logger.debug("adding response hold on %s" % channel)
					h.expire_time = int(time.time()) + timeout
					h.channel_filters[channel] = filters
					if prev_id is not None:
						last_id = response_lastids.get(channel)
						if last_id is not None and last_id != prev_id:
							del response_lastids[channel]
							lock.release()
							stats_lock.acquire()
							ci.linger = True
							ci.last_keepalive = int(time.time())
							stats_lock.release()
							# note: we don't need to do a handoff here because we didn't ack to take over yet
							logger.debug("lastid inconsistency (got=%s, expected=%s), retrying" % (prev_id, last_id))
							r = dict()
							r["requests"] = [req] # only retry the request that failed the check
							r["request-data"] = m["request-data"]
							if "inspect" in m:
								r["inspect"] = m["inspect"]
							logger.debug("OUT retry: %s" % r)
							r_raw = tnetstring.dumps(r)
							retry_sock.send(r_raw)
							quit = True
							break
					hchannel = response_channels.get(channel)
					if not hchannel:
						hchannel = dict()
						response_channels[channel] = hchannel
					hchannel[rid] = h
					req_channels = channels_by_req.get(rid)
					if not req_channels:
						req_channels = set()
						channels_by_req[rid] = req_channels
					req_channels.add((mode, channel))
					sub_key = (mode, channel)
					sub = subs.get(sub_key)
					if not sub:
						sub = Subscription(mode, channel)
						sub.last_keepalive = now
						subs[sub_key] = sub
						notify_subs.add(sub_key)
					sub.expire_time = None
				if quit:
					# we already unlocked if this is set
					continue
				lock.release()

				# ack
				out = dict()
				out['from'] = instance_id
				out['id'] = rid[1]
				out['type'] = 'keep-alive'
				m_raw = rid[0] + ' T' + tnetstring.dumps(out)
				logger.debug('OUT publish (ack response accept): %s' % m_raw)
				out_sock.send(m_raw)*/
			}
			else // StreamHold
			{
				/* # initial reply
				if "code" in response:
					rcode = response["code"]
				else:
					rcode = 200

				if "reason" in response:
					rreason = response["reason"]
				else:
					rreason = get_reason(rcode)

				if "headers" in response:
					rheaders = response["headers"]
				else:
					rheaders = dict()

				if h.auto_cross_origin:
					apply_cors_headers(h.request["headers"], rheaders)

				h.lock.acquire()
				body_size = reply_http(out_sock, rid, rcode, rreason, rheaders, response.get("body"), True, h.out_seq)
				h.out_credits -= body_size
				h.out_seq += 1
				h.lock.release()*/

				// TODO
				ZhttpRequest::ServerState ss;
				ss.rid = ZhttpRequest::Rid(rs.rid.first, rs.rid.second);
				ss.inSeq = rs.inSeq;
				ss.outSeq = rs.outSeq;
				ss.outCredits = 200000;

				ZhttpRequest *outReq = zhttpIn->createRequestFromState(ss);
				instruct.response.headers.removeAll("Content-Length");

				Hold *hold = new Hold;
				hold->mode = instruct.holdMode;
				hold->req = outReq;
				foreach(const Instruct::Channel &c, instruct.channels)
				{
					log_debug("adding stream hold on %s", qPrintable(c.name));
					hold->channels += c.name;
				}
				cs->holds += hold;

				//connect(hold->req, SIGNAL(
				outReq->beginResponse(instruct.response.code, instruct.response.reason, instruct.response.headers);
				outReq->writeBody(instruct.response.body);

				/*# bind channels
				lock.acquire()
				for channel, prev_id, filters in channels:
					logger.debug("adding stream hold on %s" % channel)
					h.channel_filters[channel] = filters
					hchannel = stream_channels.get(channel)
					if not hchannel:
						hchannel = dict()
						stream_channels[channel] = hchannel
					hchannel[rid] = h
					req_channels = channels_by_req.get(rid)
					if not req_channels:
						req_channels = set()
						channels_by_req[rid] = req_channels
					req_channels.add((mode, channel))
					sub_key = (mode, channel)
					sub = subs.get(sub_key)
					if not sub:
						sub = Subscription(mode, channel)
						sub.last_keepalive = now
						subs[sub_key] = sub
						notify_subs.add(sub_key)
					sub.expire_time = None
				lock.release()*/
			}

			/*for sub_key in notify_subs:
				out = dict()
				out['from'] = instance_id
				out['mode'] = ensure_utf8(sub_key[0])
				out['channel'] = ensure_utf8(sub_key[1])
				out['ttl'] = SUBSCRIPTION_TTL
				stats_sock.send('sub ' + tnetstring.dumps(out))
			*/
		}

		setFinished(true);
	}

private slots:
	void sessionDetectRulesSet_finished(const DeferredResult &result)
	{
		if(!result.success)
			log_debug("couldn't store detection rules");

		afterSetRules();
	}

	void sessionCreateOrUpdate_finished(const DeferredResult &result)
	{
		if(!result.success)
			log_debug("couldn't create/update session");

		afterSessionCalls();
	}
};

class Engine::Private : public QObject
{
	Q_OBJECT

public:
	Engine *q;
	Configuration config;
	ZhttpManager *zhttpIn;
	ZrpcManager *inspectServer;
	ZrpcManager *acceptServer;
	ZrpcManager *stateClient;
	ZrpcManager *controlServer;
	QZmq::Socket *inPullSock;
	QZmq::Valve *inPullValve;
	QZmq::Socket *inSubSock;
	QZmq::Valve *inSubValve;
	HttpServer *controlHttpServer;
	CommonState cs;

	Private(Engine *_q) :
		QObject(_q),
		q(_q),
		zhttpIn(0),
		inspectServer(0),
		acceptServer(0),
		stateClient(0),
		controlServer(0),
		inPullSock(0),
		inPullValve(0),
		inSubSock(0),
		inSubValve(0),
		controlHttpServer(0)
	{
		qRegisterMetaType<DetectRuleList>();
	}

	~Private()
	{
	}

	bool start(const Configuration &_config)
	{
		config = _config;

		zhttpIn = new ZhttpManager(this);

		zhttpIn->setInstanceId(config.instanceId);
		zhttpIn->setServerInStreamSpecs(config.serverInStreamSpecs);
		zhttpIn->setServerOutSpecs(config.serverOutSpecs);

		log_info("zhttp in stream: %s", qPrintable(config.serverInStreamSpecs.join(", ")));
		log_info("zhttp out: %s", qPrintable(config.serverOutSpecs.join(", ")));

		if(!config.inspectSpec.isEmpty())
		{
			inspectServer = new ZrpcManager(this);
			inspectServer->setBind(false);
			inspectServer->setIpcFileMode(config.ipcFileMode);
			connect(inspectServer, SIGNAL(requestReady()), SLOT(inspectServer_requestReady()));

			if(!inspectServer->setServerSpecs(QStringList() << config.inspectSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("inspect server: %s", qPrintable(config.inspectSpec));
		}

		if(!config.acceptSpec.isEmpty())
		{
			acceptServer = new ZrpcManager(this);
			acceptServer->setBind(false);
			acceptServer->setIpcFileMode(config.ipcFileMode);
			connect(acceptServer, SIGNAL(requestReady()), SLOT(acceptServer_requestReady()));

			if(!acceptServer->setServerSpecs(QStringList() << config.acceptSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("accept server: %s", qPrintable(config.acceptSpec));
		}

		if(!config.stateSpec.isEmpty())
		{
			stateClient = new ZrpcManager(this);
			stateClient->setBind(true);
			stateClient->setIpcFileMode(config.ipcFileMode);

			if(!stateClient->setClientSpecs(QStringList() << config.stateSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("state client: %s", qPrintable(config.stateSpec));
		}

		if(!config.commandSpec.isEmpty())
		{
			controlServer = new ZrpcManager(this);
			controlServer->setBind(true);
			controlServer->setIpcFileMode(config.ipcFileMode);
			connect(controlServer, SIGNAL(requestReady()), SLOT(controlServer_requestReady()));

			if(!controlServer->setServerSpecs(QStringList() << config.commandSpec))
			{
				// zrpcmanager logs error
				return false;
			}

			log_info("control server: %s", qPrintable(config.commandSpec));
		}

		if(!config.pushInSpec.isEmpty())
		{
			inPullSock = new QZmq::Socket(QZmq::Socket::Pull, this);
			inPullSock->setHwm(DEFAULT_HWM);

			QString errorMessage;
			if(!ZUtil::setupSocket(inPullSock, config.pushInSpec, true, config.ipcFileMode, &errorMessage))
			{
					log_error("%s", qPrintable(errorMessage));
					return false;
			}

			inPullValve = new QZmq::Valve(inPullSock, this);
			connect(inPullValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(inPull_readyRead(const QList<QByteArray> &)));

			log_info("in pull: %s", qPrintable(config.pushInSpec));
		}

		if(!config.pushInSubSpec.isEmpty())
		{
			// FIXME: XSub
			inSubSock = new QZmq::Socket(QZmq::Socket::Sub, this);
			inSubSock->setSendHwm(SUB_SNDHWM);

			QString errorMessage;
			if(!ZUtil::setupSocket(inSubSock, config.pushInSubSpec, true, config.ipcFileMode, &errorMessage))
			{
					log_error("%s", qPrintable(errorMessage));
					return false;
			}

			inSubValve = new QZmq::Valve(inSubSock, this);
			connect(inSubValve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(inSub_readyRead(const QList<QByteArray> &)));

			log_info("in sub: %s", qPrintable(config.pushInSubSpec));
		}

		if(config.pushInHttpPort != -1)
		{
			controlHttpServer = new HttpServer(this);
			connect(controlHttpServer, SIGNAL(requestReady()), SLOT(controlHttpServer_requestReady()));
			controlHttpServer->listen(config.pushInHttpAddr, config.pushInHttpPort);

			log_info("http control server: %s:%d", qPrintable(config.pushInHttpAddr.toString()), config.pushInHttpPort);
		}

		if(inPullValve)
			inPullValve->open();
		if(inSubValve)
			inSubValve->open();

		return true;
	}

	void reload()
	{
		// nothing to do
	}

private:
	void handlePublishItem(const PublishItem &item)
	{
		log_debug("item to publish: channel=%s, formats=%d", qPrintable(item.channel), item.formats.count());

		foreach(Hold *hold, cs.holds)
		{
			if(!hold->req)
				continue;

			bool found = false;
			foreach(const QString &c, hold->channels)
			{
				if(c == item.channel)
				{
					found = true;
					break;
				}
			}
			if(!found)
				continue;

			if(hold->mode == ResponseHold && item.formats.contains(Format::HttpResponse))
			{
				log_debug("relaying to 1 http-response subscribers");
				Format f = item.formats.value(Format::HttpResponse);

				// TODO: extract Grip-Expose-Headers header

				// inherit headers
				HttpHeaders headers = hold->response.headers;
				headers.removeAll("Content-Length"); // this will be auto set

				// merge
				foreach(const HttpHeader &h, f.headers)
				{
					headers.removeAll(h.first);
					headers += h;
				}

				// TODO: if Grip-Expose-Headers was set, apply now

				// TODO: support body patch

				hold->req->beginResponse(f.code, f.reason, headers);
				hold->req->writeBody(f.body);
				hold->req->endBody();
				hold->req = 0;
			}
			else if(hold->mode == StreamHold && item.formats.contains(Format::HttpStream))
			{
				log_debug("relaying to 1 http-stream subscribers");
				Format f = item.formats.value(Format::HttpStream);

				if(f.close)
				{
					hold->req->endBody();
					hold->req = 0;
				}
				else
					hold->req->writeBody(f.body);
			}
		}

		/*
		out_sock = ctx.socket(zmq.PUB)
		out_sock.linger = DEFAULT_LINGER
		for spec in m2a_out_specs:
			out_sock.connect(spec)

		ws_control_out_sock = ctx.socket(zmq.PUSH)
		ws_control_out_sock.linger = DEFAULT_LINGER
		ws_control_out_sock.connect(ws_control_out_spec)

		stats_sock = ctx.socket(zmq.PUSH)
		stats_sock.linger = 0
		stats_sock.connect('inproc://stats_in')

		if state_spec:
			state_rpc = rpc.RpcClient(['inproc://state'], context=ctx)
		else:
			state_rpc = None

		while True:
			m_raw = in_sock.recv()
			m = tnetstring.loads(m_raw)
			logger.debug("IN publish: %s" % m)
			channel = m["channel"]
			id = m.get("id", None)
			formats = m["formats"]
			meta = m.get("meta", {})

			response_holds = list()
			stream_holds = list()
			ws_cids = list()
			sids = set()
			notify_unsubs = set()

			if "http-response" in formats:
				lock.acquire()
				hchannel = response_channels.get(channel)
				if hchannel:
					all_response_holds = hchannel.values()
					unsubs = set()
					skipped_some = False
					for h in all_response_holds:
						if not apply_filters(h.meta, meta, h.channel_filters[channel]):
							skipped_some = True
							continue
						response_holds.append(h)
						unsub_list = remove_from_response_channels(h.rid)
						for sub_key in unsub_list:
							unsubs.add(sub_key)
					assert(skipped_some or channel not in response_channels)
					for sub_key in unsubs:
						sub = subs.get(sub_key)
						if sub and sub.expire_time is None:
							# flag for deletion soon
							sub.expire_time = int(time.time()) + SUBSCRIPTION_LINGER
				item_id = m.get("id")
				if item_id is not None:
					response_lastids[channel] = item_id
				lock.release()
				stats_lock.acquire()
				for h in response_holds:
					ci = conns.get("%s:%s" % (h.rid[0], h.rid[1]))
					if ci is not None and ci.sid:
						sids.add(ci.sid)
				stats_lock.release()

			if "http-stream" in formats:
				do_close = (formats["http-stream"].get("action") == "close")

				lock.acquire()
				hchannel = stream_channels.get(channel)
				if hchannel:
					all_stream_holds = hchannel.values()
					unsubs = set()
					skipped_some = False
					for h in all_stream_holds:
						if not apply_filters(h.meta, meta, h.channel_filters[channel]):
							skipped_some = True
							continue
						stream_holds.append(h)
						if do_close:
							unsub_list = remove_from_stream_channels(h.rid)
							for sub_key in unsub_list:
								unsubs.add(sub_key)
					assert(not do_close or skipped_some or channel not in stream_channels)
					for sub_key in unsubs:
						sub = subs.get(sub_key)
						if sub:
							del subs[sub_key]
							notify_unsubs.add(sub_key)
				lock.release()
				stats_lock.acquire()
				for h in stream_holds:
					ci = conns.get("%s:%s" % (h.rid[0], h.rid[1]))
					if ci is not None and ci.sid:
						sids.add(ci.sid)
				stats_lock.release()

			if "ws-message" in formats:
				lock.acquire()
				hchannel = ws_channels.get(channel)
				if hchannel:
					for cid, sess in hchannel.iteritems():
						if not apply_filters(sess.meta, meta, sess.channel_filters[channel]):
							continue
						ws_cids.append(cid)
						if sess.sid:
							sids.add(sess.sid)
				lock.release()

			# update sessions' last-id
			if id is not None and sids:
				sid_last_ids = dict()
				for sid in sids:
					sid_last_ids[sid] = {channel: id}

				if sid_last_ids and state_rpc:
					try:
						session_update_many(state_rpc, sid_last_ids)
					except:
						logger.debug("couldn't update sessions")

			if response_holds:
				logger.debug("relaying to %d http-response subscribers" % len(response_holds))
				http_response = formats["http-response"]

				if "code" in http_response:
					pcode = http_response["code"]
				else:
					pcode = 200

				if "reason" in http_response:
					preason = http_response["reason"]
				else:
					preason = get_reason(pcode)

				if "headers" in http_response:
					pheaders = http_response["headers"]
					if isinstance(pheaders, list):
						d = dict()
						for i in pheaders:
							d[i[0]] = i[1]
						pheaders = d
				else:
					pheaders = dict()

				if "body" in http_response:
					pbody = http_response["body"]
				else:
					pbody = ""

				if "body-patch" in http_response:
					pbody_patch = http_response["body-patch"]
				else:
					pbody_patch = None

				grip_expose_headers = header_get_all(pheaders, 'Grip-Expose-Headers')
				header_remove(pheaders, 'Grip-Expose-Headers')

				for n, h in enumerate(response_holds):
					# inherit any headers from the timeout response
					if 'headers' in h.response:
						rheaders = copy.deepcopy(h.response['headers'])
					else:
						rheaders = dict()

					# apply the headers from the pushed message
					for k, v in pheaders.iteritems():
						header_set(rheaders, k, v)

					# if Grip-Expose-Headers was provided in the pushed message, filter the results
					if grip_expose_headers:
						rkeys = rheaders.keys()
						for k in rkeys:
							if not headernames_contains(grip_expose_headers, k):
								del rheaders[k]

					# if body patch specified, inherit body from timeout response
					if pbody_patch is not None:
						try:
							rbody = json.loads(h.response['body'])
							rbody = json_patch(rbody, pbody_patch)
							rbody = json.dumps(rbody)
						except Exception as e:
							logger.debug("failed to parse json patch: %s", e.message)
							rbody = ''
					else:
						rbody = pbody

					headers = dict()
					if h.jsonp_callback:
						if h.jsonp_extended_response:
							result = dict()
							result["code"] = pcode
							result["reason"] = preason
							result["headers"] = dict()
							if rheaders:
								for k, v in rheaders.iteritems():
									result["headers"][k] = v
							header_set(result["headers"], "Content-Length", str(len(pbody)))
							result["body"] = rbody

							body = h.jsonp_callback + "(" + json.dumps(result) + ");\n"
						else:
							body = h.jsonp_callback + "(" + rbody + ");\n"

						header_set(headers, "Content-Type", "application/javascript")
						header_set(headers, "Content-Length", str(len(body)))
						reply_http(out_sock, h.rid, 200, "OK", headers, body)
					else:
						if rheaders:
							for k, v in rheaders.iteritems():
								headers[k] = v

						if h.auto_cross_origin:
							apply_cors_headers(h.request["headers"], headers)

						reply_http(out_sock, h.rid, pcode, preason, headers, rbody)

					# report request done

					stats_lock.acquire()
					ci = conns.get("%s:%s" % (h.rid[0], h.rid[1]))
					if ci is not None:
						ci = copy.deepcopy(ci)
						del conns[ci.id]
					stats_lock.release()

					if ci is not None:
						out = dict()
						out['from'] = instance_id
						if ci.route:
							out['route'] = ci.route
						out['id'] = ci.id
						out['unavailable'] = True
						stats_sock.send('conn ' + tnetstring.dumps(out))

					if n % SEND_BATCH_SIZE == 0:
						time.sleep(SEND_BATCH_DELAY)

				rcount = len(response_holds)
				if rcount > 0:
					out = dict()
					out['from'] = instance_id
					out['channel'] = ensure_utf8(channel)
					if 'id' in m:
						out['item-id'] = ensure_utf8(m['id'])
					out['count'] = rcount
					out['transport'] = 'http-response'
					stats_sock.send('message ' + tnetstring.dumps(out))

			if stream_holds:
				content = formats["http-stream"].get("content")
				if content or do_close:
					logger.debug("relaying to %d http-stream subscribers" % len(stream_holds))
					for n, h in enumerate(stream_holds):
						if content:
							if h.out_credits < len(content):
								logger.debug('not enough send credits, dropping')
								continue
							h.lock.acquire()
							reply_http_chunk(out_sock, h.rid, content, h.out_seq)
							h.out_credits -= len(content)
							h.out_seq += 1
							h.lock.release()
							lock.acquire()
							h.last_send = int(time.time())
							lock.release()
						if do_close:
							reply_http_close(out_sock, h.rid)

							# report request done

							stats_lock.acquire()
							ci = conns.get("%s:%s" % (h.rid[0], h.rid[1]))
							if ci is not None:
								ci = copy.deepcopy(ci)
								del conns[ci.id]
							stats_lock.release()

							if ci is not None:
								out = dict()
								out['from'] = instance_id
								if ci.route:
									out['route'] = ci.route
								out['id'] = ci.id
								out['unavailable'] = True
								stats_sock.send('conn ' + tnetstring.dumps(out))

						if n % SEND_BATCH_SIZE == 0:
							time.sleep(SEND_BATCH_DELAY)

				rcount = len(stream_holds)
				if rcount > 0:
					out = dict()
					out['from'] = instance_id
					out['channel'] = ensure_utf8(channel)
					if 'id' in m:
						out['item-id'] = ensure_utf8(m['id'])
					out['count'] = rcount
					out['transport'] = 'http-stream'
					stats_sock.send('message ' + tnetstring.dumps(out))

			if ws_cids:
				logger.debug("relaying to %d ws-message subscribers" % len(ws_cids))
				for n, cid in enumerate(ws_cids):
					t = formats['ws-message']
					if 'content-bin' in t:
						content_type = 'binary'
						content = t['content-bin']
					elif "content" in t:
						content_type = 'text'
						content = t['content']
					else:
						content = None

					if content is not None:
						item = dict()
						item['cid'] = cid
						item['type'] = 'send'
						item['content-type'] = content_type
						item['message'] = content
						out = dict()
						out['items'] = [item]
						logger.debug('OUT wscontrol: %s' % out)
						ws_control_out_sock.send(tnetstring.dumps(out))

					if n % SEND_BATCH_SIZE == 0:
						time.sleep(SEND_BATCH_DELAY)

				rcount = len(ws_cids)
				if rcount > 0:
					out = dict()
					out['from'] = instance_id
					out['channel'] = ensure_utf8(channel)
					if 'id' in m:
						out['item-id'] = ensure_utf8(m['id'])
					out['count'] = rcount
					out['transport'] = 'ws-message'
					stats_sock.send('message ' + tnetstring.dumps(out))

			for sub_key in notify_unsubs:
				out = dict()
				out['from'] = instance_id
				out['mode'] = ensure_utf8(sub_key[0])
				out['channel'] = ensure_utf8(sub_key[1])
				out['unavailable'] = True
				stats_sock.send('sub ' + tnetstring.dumps(out))
		*/
	}

private slots:
	void inspectServer_requestReady()
	{
		ZrpcRequest *req = inspectServer->takeNext();
		if(!req)
			return;

		new InspectWorker(req, stateClient, config.shareAll, this);
	}

	void acceptServer_requestReady()
	{
		ZrpcRequest *req = acceptServer->takeNext();
		if(!req)
			return;

		new AcceptWorker(req, stateClient, &cs, zhttpIn, this);
	}

	void controlServer_requestReady()
	{
		ZrpcRequest *req = controlServer->takeNext();
		if(!req)
			return;

		log_debug("IN command: %s args=%s", qPrintable(req->method()), qPrintable(TnetString::variantToString(req->args(), -1)));

		if(req->method() == "conncheck")
		{
			// TODO
			req->respondError("not-implemented");
			/*if 'ids' not in args or not isinstance(args['ids'], list):
				raise rpc.CallError('bad-format')
			cids = set(args['ids'])

			stats_lock.acquire()
			missing = set()
			for cid in cids:
				if cid not in conns:
					missing.add(cid)
			stats_lock.release()

			if len(missing) > 0:
				try:
					found = proxy_client.call('conncheck', {'ids': list(missing)})
				except:
					raise rpc.CallError('proxy-request-failed')
				for cid in found:
					missing.remove(cid)

			for cid in missing:
				cids.remove(cid)

			return list(cids)*/
		}
		else if(req->method() == "get-zmq-uris")
		{
			QVariantHash out;
			if(!config.commandSpec.isEmpty())
				out["command"] = config.commandSpec.toUtf8();
			if(!config.pushInSpec.isEmpty())
				out["publish-pull"] = config.pushInSpec.toUtf8();
			if(!config.pushInSubSpec.isEmpty())
				out["publish-sub"] = config.pushInSubSpec.toUtf8();
			req->respond(out);
		}
		else
			req->respondError("method-not-found");

		delete req;
	}

	void inPull_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("IN pull: received message with parts != 1, skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(message[0], 0, &ok);
		if(!ok)
		{
			log_warning("IN pull: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		QString errorMessage;
		PublishItem item = PublishItem::fromVariant(data, &ok, &errorMessage);
		if(!ok)
		{
			log_warning("IN pull: received message with invalid format: %s, skipping", qPrintable(errorMessage));
			return;
		}

		handlePublishItem(item);
	}

	void inSub_readyRead(const QList<QByteArray> &message)
	{
		// TODO
		Q_UNUSED(message);

		/*
		sub_cmd_in_sock = ctx.socket(zmq.PULL)
		sub_cmd_in_sock.connect("inproc://sub_cmd_in")

		out_sock = ctx.socket(zmq.PUSH)
		out_sock.linger = DEFAULT_LINGER
		out_sock.connect("inproc://push_in")

		poller = zmq.Poller()
		poller.register(in_sock, zmq.POLLIN)
		poller.register(sub_cmd_in_sock, zmq.POLLIN)

		while True:
			socks = dict(poller.poll())
			if socks.get(in_sock) == zmq.POLLIN:
				m_raw = in_sock.recv_multipart()

				try:
					try:
						m = tnetstring.loads(m_raw[1])
					except:
						raise ValidationError("bad format (not a tnetstring)")

					m['channel'] = m_raw[0]
					m = validate_publish(m)

				except ValidationError as e:
					logger.debug("warning: %s, dropping" % e.message)
					continue

				out_sock.send(tnetstring.dumps(m))
			elif socks.get(sub_cmd_in_sock) == zmq.POLLIN:
				m = tnetstring.loads(sub_cmd_in_sock.recv())
				mtype = m['type']
				channel = m['channel']
				if mtype == 'subscribe':
					logger.debug('SUB socket subscribe: %s' % channel)
					in_sock.setsockopt(zmq.SUBSCRIBE, channel)
				elif mtype == 'unsubscribe':
					logger.debug('SUB socket unsubscribe: %s' % channel)
					in_sock.setsockopt(zmq.UNSUBSCRIBE, channel)
		*/
	}

	void controlHttpServer_requestReady()
	{
		HttpRequest *req = controlHttpServer->takeNext();
		if(!req)
			return;

		if(req->requestUri() == "/publish/" || req->requestUri() == "/publish")
		{
			if(req->requestMethod() == "POST")
			{
				QJson::Parser parser;
				bool ok;
				QVariant vdata = parser.parse(req->requestBody(), &ok);
				if(req->requestBody().isEmpty() || !ok)
				{
					req->respond(400, "Bad Request", "Body is not valid JSON.\n");
					connect(req, SIGNAL(finished()), req, SLOT(deleteLater()));
					return;
				}

				if(vdata.type() != QVariant::Map)
				{
					req->respond(400, "Bad Request", "Invalid format.\n");
					connect(req, SIGNAL(finished()), req, SLOT(deleteLater()));
					return;
				}

				QVariantMap mdata = vdata.toMap();
				QVariantList vitems;

				if(!mdata.contains("items"))
				{
					req->respond(400, "Bad Request", "Invalid format: object does not contain 'items'\n");
					connect(req, SIGNAL(finished()), req, SLOT(deleteLater()));
					return;
				}

				if(mdata["items"].type() != QVariant::List)
				{
					req->respond(400, "Bad Request", "Invalid format: object contains 'items' with wrong type\n");
					connect(req, SIGNAL(finished()), req, SLOT(deleteLater()));
					return;
				}

				vitems = mdata["items"].toList();

				QString errorMessage;
				QList<PublishItem> items = parseHttpItems(vitems, &ok, &errorMessage);
				if(!ok)
				{
					req->respond(400, "Bad Request", QString("Invalid format: %1\n").arg(errorMessage));
					connect(req, SIGNAL(finished()), req, SLOT(deleteLater()));
					return;
				}

				foreach(const PublishItem &item, items)
					handlePublishItem(item);

				req->respond(200, "OK", "Published\n");
			}
			else
			{
				HttpHeaders headers;
				headers += HttpHeader("Content-Type", "text/plain");
				headers += HttpHeader("Allow", "POST");
				req->respond(405, "Method Not Allowed", headers, "Method not allowed: " + req->requestMethod().toUtf8() + ".\n");
			}
		}
		else
		{
			req->respond(404, "Not Found", "Not Found\n");
		}

		connect(req, SIGNAL(finished()), req, SLOT(deleteLater()));
	}
};

Engine::Engine(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

Engine::~Engine()
{
	delete d;
}

bool Engine::start(const Configuration &config)
{
	return d->start(config);
}

void Engine::reload()
{
	d->reload();
}

#include "engine.moc"

/*
def session_worker():
	in_sock = ctx.socket(zmq.DEALER)
	in_sock.identity = instance_id
	for spec in m2a_in_stream_specs:
		in_sock.connect(spec)

	out_sock = ctx.socket(zmq.PUB)
	out_sock.linger = DEFAULT_LINGER
	for spec in m2a_out_specs:
		out_sock.connect(spec)

	stats_sock = ctx.socket(zmq.PUSH)
	stats_sock.linger = 0
	stats_sock.connect('inproc://stats_in')

	while True:
		m_list = in_sock.recv_multipart()
		m = tnetstring.loads(m_list[1][1:])
		logger.debug('IN session: %s' % m)
		mtype = m.get('type')
		notify_unsubs = set()
		if mtype is not None and (mtype == 'error' or mtype == 'cancel'):
			rid = (m['from'], m['id'])
			logger.debug('cleaning up subscriber %s' % repr(rid))
			now = int(time.time())
			lock.acquire()
			unsub_list = remove_from_req_channels(rid)
			for sub_key in unsub_list:
				sub = subs.get(sub_key)
				if sub:
					if sub.mode == 'response' and sub.expire_time is None:
						# flag for deletion soon
						sub.expire_time = now + SUBSCRIPTION_LINGER
					elif sub.mode == 'stream':
						del subs[sub_key]
						notify_unsubs.add(sub_key)
			lock.release()

			stats_lock.acquire()
			ci = conns.get("%s:%s" % (rid[0], rid[1]))
			if ci is not None:
				ci = copy.deepcopy(ci)
				del conns[ci.id]
			stats_lock.release()

			if ci is not None:
				out = dict()
				out['from'] = instance_id
				if ci.route:
					out['route'] = ci.route
				out['id'] = ci.id
				out['unavailable'] = True
				stats_sock.send('conn ' + tnetstring.dumps(out))

		elif mtype is not None:
			# is this a known session?
			rid = (m['from'], m['id'])

			h = None
			lock.acquire()
			for hchannels in response_channels.itervalues():
				if rid in hchannels:
					h = hchannels[rid]
					break
			if h is None:
				for hchannels in stream_channels.itervalues():
					if rid in hchannels:
						h = hchannels[rid]
						break
			lock.release()

			if h is not None:
				if mtype == 'credit':
					credits = m['credits']
					lock.acquire()
					h.out_credits += credits
					lock.release()
					logger.debug('received %d credits, now %d' % (credits, h.out_credits))
			else:
				# no such session, send cancel
				out = dict()
				out['from'] = instance_id
				out['id'] = m['id']
				out['type'] = 'cancel'
				m_raw = m['from'] + ' T' + tnetstring.dumps(out)
				logger.debug('OUT publish: %s' % m_raw)
				out_sock.send(m_raw)

		for sub_key in notify_unsubs:
			out = dict()
			out['from'] = instance_id
			out['mode'] = ensure_utf8(sub_key[0])
			out['channel'] = ensure_utf8(sub_key[1])
			out['unavailable'] = True
			stats_sock.send('sub ' + tnetstring.dumps(out))

def ws_control_worker():
	in_sock = ctx.socket(zmq.PULL)
	in_sock.connect(ws_control_in_spec)

	out_sock = ctx.socket(zmq.PUSH)
	out_sock.linger = DEFAULT_LINGER
	out_sock.connect(ws_control_out_spec)

	stats_sock = ctx.socket(zmq.PUSH)
	stats_sock.linger = 0
	stats_sock.connect('inproc://stats_in')

	if state_spec:
		state_rpc = rpc.RpcClient(['inproc://state'], context=ctx)
	else:
		state_rpc = None

	while True:
		m_raw = in_sock.recv()
		m = tnetstring.loads(m_raw)
		logger.debug('IN wscontrol: %s' % m)

		now = int(time.time())

		for item in m['items']:
			mtype = item.get('type')
			if mtype == 'here':
				cid = item['cid']
				channel_prefix = item.get('channel-prefix')
				lock.acquire()
				s = ws_sessions.get(cid)
				if not s:
					s = WsSession(cid)
					s.channel_prefix = channel_prefix
					ws_sessions[cid] = s
					logger.debug('added ws session: %s' % cid)
				s.expire_time = now + 60
				lock.release()
			elif mtype == 'gone' or mtype == 'cancel':
				cid = item['cid']

				notify_unsubs = set()

				lock.acquire()
				s = ws_sessions.get(cid)
				if s:
					del ws_sessions[cid]
					channels = set()
					for channel, hchannels in ws_channels.iteritems():
						channels.add(channel)
						if cid in hchannels:
							del hchannels[cid]
					for channel in channels:
						if channel in ws_channels and len(ws_channels[channel]) == 0:
							del ws_channels[channel]
							sub_key = ('ws', channel)
							sub = subs.get(sub_key)
							if sub:
								del subs[sub_key]
								notify_unsubs.add(sub_key)
					logger.debug('removed ws session: %s' % cid)
				lock.release()

				for sub_key in notify_unsubs:
					out = dict()
					out['from'] = instance_id
					out['mode'] = ensure_utf8(sub_key[0])
					out['channel'] = ensure_utf8(sub_key[1])
					out['unavailable'] = True
					stats_sock.send('sub ' + tnetstring.dumps(out))

			elif mtype == 'grip':
				cid = item['cid']

				try:
					gm = json.loads(item['message'])
					gtype = gm['type']
					channel = None
					filters = []
					sid = None
					name = None
					value = None
					if gtype == 'subscribe' or gtype == 'unsubscribe':
						channel = gm['channel']
						if not isinstance(channel, basestring) or len(channel) < 1:
							raise ValueError('invalid channel')
						if gtype == 'subscribe':
							filters = gm.get('filters', [])
							if not isinstance(filters, list):
								raise ValueError('invalid filters')
							for f in filters:
								if not isinstance(f, basestring):
									raise ValueError('invalid filters')
					elif gtype == 'session':
						sid = gm['id']
						if not isinstance(sid, basestring) or len(sid) < 1:
							raise ValueError('invalid id')
						sid = ensure_utf8(sid)
					elif gtype == 'set-meta':
						name = gm['name']
						if not isinstance(name, basestring) or len(name) < 1:
							raise ValueError('invalid meta name')
						value = gm.get('value')
						if value is not None and not isinstance(value, basestring):
							raise ValueError('invalid meta value')
				except:
					gm = None

				notify_sub = False
				notify_unsub = False
				notify_detach = False
				save_sid = False

				if gm:
					lock.acquire()
					s = ws_sessions.get(cid)
					if s:
						if channel is not None and s.channel_prefix:
							channel = s.channel_prefix + channel
						if gtype == 'subscribe':
							s.channel_filters[channel] = filters
							hchannel = ws_channels.get(channel)
							if not hchannel:
								hchannel = dict()
								ws_channels[channel] = hchannel
							hchannel[cid] = s
							sub_key = ('ws', channel)
							sub = subs.get(sub_key)
							if not sub:
								sub = Subscription('ws', channel)
								sub.last_keepalive = now
								subs[sub_key] = sub
								notify_sub = True
							sub.expire_time = None
						elif gtype == 'unsubscribe':
							if channel in s.channel_filters:
								del s.channel_filters[channel]
							hchannel = ws_channels.get(channel)
							if hchannel:
								if cid in hchannel:
									del hchannel[cid]
									if len(hchannel) == 0:
										del ws_channels[channel]
									sub_key = ('ws', channel)
									sub = subs.get(sub_key)
									if sub:
										del subs[sub_key]
										notify_unsub = True
						elif gtype == 'detach':
							notify_detach = True
						elif gtype == 'session':
							s.sid = sid
							save_sid = True
						elif gtype == 'set-meta':
							if not value and name in s.meta:
								del s.meta[name]
							elif value:
								s.meta[name] = value
					lock.release()

				if state_rpc and save_sid:
					try:
						session_create_or_update(state_rpc, sid, {})
					except:
						logger.debug("couldn't create/update session")

				if notify_sub:
					logger.debug('ws session %s subscribed to %s' % (cid, channel))
					out = dict()
					out['from'] = instance_id
					out['mode'] = 'ws'
					out['channel'] = ensure_utf8(channel)
					out['ttl'] = SUBSCRIPTION_TTL
					stats_sock.send('sub ' + tnetstring.dumps(out))

				if notify_unsub:
					out = dict()
					out['from'] = instance_id
					out['mode'] = 'ws'
					out['channel'] = ensure_utf8(channel)
					out['unavailable'] = True
					stats_sock.send('sub ' + tnetstring.dumps(out))

				if notify_detach:
					item = dict()
					item['cid'] = cid
					item['type'] = 'detach'
					out = dict()
					out['items'] = [item]
					logger.debug('OUT wscontrol: %s' % out)
					out_sock.send(tnetstring.dumps(out))

def timeout_worker():
	out_sock = ctx.socket(zmq.PUB)
	out_sock.linger = DEFAULT_LINGER
	for spec in m2a_out_specs:
		out_sock.connect(spec)

	stats_sock = ctx.socket(zmq.PUSH)
	stats_sock.linger = 0
	stats_sock.connect('inproc://stats_in')

	if state_spec:
		state_rpc = rpc.RpcClient(['inproc://state'], context=ctx)
	else:
		state_rpc = None

	while True:
		now = int(time.time())

		lock.acquire()
		holds = list()
		for hchannels in response_channels.itervalues():
			for h in hchannels.values():
				if h.expire_time and now >= h.expire_time:
					holds.append(h)
		holds_by_rid = dict()
		for h in holds:
			if h.rid not in holds_by_rid:
				holds_by_rid[h.rid] = h
			unsub_list = remove_from_response_channels(h.rid)
			for sub_key in unsub_list:
				sub = subs.get(sub_key)
				if sub and sub.expire_time is None:
					# flag for deletion soon
					sub.expire_time = now + SUBSCRIPTION_LINGER
		lock.release()

		if len(holds_by_rid) > 0:
			logger.debug("timing out %d subscribers" % len(holds_by_rid))

			for h in holds_by_rid.itervalues():
				if "code" in h.response:
					pcode = h.response["code"]
				else:
					pcode = 200

				if "reason" in h.response:
					preason = h.response["reason"]
				else:
					preason = get_reason(pcode)

				if "headers" in h.response:
					pheaders = h.response["headers"]
				else:
					pheaders = dict()

				if "body" in h.response:
					pbody = h.response["body"]
				else:
					pbody = ""

				headers = dict()
				if h.jsonp_callback:
					if h.jsonp_extended_response:
						result = dict()
						result["code"] = pcode
						result["reason"] = preason
						result["headers"] = dict()
						if pheaders:
							for k, v in pheaders.iteritems():
								result["headers"][k] = v
						header_set(result["headers"], "Content-Length", str(len(pbody)))
						result["body"] = pbody

						body = h.jsonp_callback + "(" + json.dumps(result) + ");\n"
					else:
						body = h.jsonp_callback + "(" + pbody + ");\n"

					header_set(headers, "Content-Type", "application/javascript")
					header_set(headers, "Content-Length", str(len(body)))
					reply_http(out_sock, h.rid, 200, "OK", headers, body)
				else:
					if pheaders:
						for k, v in pheaders.iteritems():
							headers[k] = v

					if h.auto_cross_origin:
						apply_cors_headers(h.request["headers"], headers)

					reply_http(out_sock, h.rid, pcode, preason, headers, pbody)

				stats_lock.acquire()
				ci = conns.get("%s:%s" % (h.rid[0], h.rid[1]))
				if ci is not None:
					ci = copy.deepcopy(ci)
					del conns[ci.id]
				stats_lock.release()

				if ci is not None:
					out = dict()
					out['from'] = instance_id
					if ci.route:
						out['route'] = ci.route
					out['id'] = ci.id
					out['unavailable'] = True
					stats_sock.send('conn ' + tnetstring.dumps(out))

		now = int(time.time())
		grip_ka_rids = dict() # (hold, content)
		ka_rids = set()
		lock.acquire()
		for channel, hchannels in response_channels.iteritems():
			for h in hchannels.values():
				if h.last_keepalive is None or h.last_keepalive + 30 < now:
					if h.rid not in ka_rids:
						h.last_keepalive = now
						ka_rids.add(h.rid)
		for channel, hchannels in stream_channels.iteritems():
			for h in hchannels.values():
				if h.grip_keep_alive and (h.last_send is None or h.last_send + h.grip_keep_alive_timeout < now):
					if h.rid not in grip_ka_rids:
						content = h.grip_keep_alive
						if h.out_credits < len(content):
							logger.debug('not enough send credits, skipping keep alive')
							continue
						h.last_send = now
						h.last_keepalive = now
						grip_ka_rids[h.rid] = (h, content)
				if h.last_keepalive is None or h.last_keepalive + 30 < now:
					if h.rid not in grip_ka_rids and h.rid not in ka_rids:
						h.last_keepalive = now
						ka_rids.add(h.rid)
		lock.release()

		if len(grip_ka_rids) > 0:
			logger.debug("keep-aliving (grip) %d subscribers" % len(grip_ka_rids))
			for rid, v in grip_ka_rids.iteritems():
				h, content = v
				h.lock.acquire()
				reply_http_chunk(out_sock, rid, content, h.out_seq)
				h.out_credits -= len(content)
				h.out_seq += 1
				h.lock.release()

		if len(ka_rids) > 0:
			logger.debug("keep-aliving %d subscribers" % len(ka_rids))
			for rid in ka_rids:
				out = dict()
				out['from'] = instance_id
				out['id'] = rid[1]
				out['type'] = 'keep-alive'
				m_raw = rid[0] + ' T' + tnetstring.dumps(out)
				logger.debug('OUT publish: %s' % m_raw)
				out_sock.send(m_raw)

		now = int(time.time())
		cids = set()
		lock.acquire()
		for cid, s in ws_sessions.iteritems():
			if s.expire_time and now >= s.expire_time:
				cids.add(cid)
		for cid in cids:
			del ws_sessions[cid]
			channels = set()
			for channel, hchannels in ws_channels.iteritems():
				channels.add(channel)
				if cid in hchannels:
					del hchannels[cid]
			for channel in channels:
				if channel in ws_channels and len(ws_channels[channel]) == 0:
					del ws_channels[channel]
					sub_key = ('ws', channel)
					sub = subs.get(sub_key)
					if sub:
						# use expire_time to flag for removal
						sub.expire_time = now
		lock.release()
		if len(cids) > 0:
			logger.debug("timing out %d ws sessions" % len(cids))

		notify_subs = set()
		notify_unsubs = set()
		lock.acquire()
		for sub_key, sub in subs.iteritems():
			if sub.expire_time is not None and now >= sub.expire_time:
				notify_unsubs.add(sub_key)
			elif sub.last_keepalive is None or sub.last_keepalive + 30 < now:
				sub.last_keepalive = now
				notify_subs.add(sub_key)
		for sub_key in notify_unsubs:
			del subs[sub_key]
		lock.release()

		for sub_key in notify_unsubs:
			out = dict()
			out['from'] = instance_id
			out['mode'] = ensure_utf8(sub_key[0])
			out['channel'] = ensure_utf8(sub_key[1])
			out['unavailable'] = True
			stats_sock.send('sub ' + tnetstring.dumps(out))

		if len(notify_subs) > 0:
			logger.debug('keep-aliving %d subscriptions' % len(notify_subs))
			for sub_key in notify_subs:
				out = dict()
				out['from'] = instance_id
				out['mode'] = ensure_utf8(sub_key[0])
				out['channel'] = ensure_utf8(sub_key[1])
				out['ttl'] = SUBSCRIPTION_TTL
				stats_sock.send('sub ' + tnetstring.dumps(out))

		refresh_conns = list()
		send_activity = list()
		stats_lock.acquire()
		remove_cids = set()
		for cid, ci in conns.iteritems():
			if ci.last_keepalive is None or (not ci.linger and ci.last_keepalive + CONNECTION_REFRESH < now) or (ci.linger and ci.last_keepalive + CONNECTION_LINGER < now):
				if ci.linger:
					remove_cids.add(cid)
				else:
					ci.last_keepalive = now
					refresh_conns.append(copy.deepcopy(ci))
		for cid in remove_cids:
			del conns[cid]
		for route, activity in stats_activity.iteritems():
			send_activity.append((route, activity))
		stats_activity.clear()
		stats_lock.release()

		refresh_sids = list()

		for ci in refresh_conns:
			out = dict()
			out['from'] = instance_id
			if ci.route:
				out['route'] = ci.route
			out['id'] = ci.id
			out['type'] = ci.type
			if ci.peer_address:
				out['peer-address'] = ci.peer_address
			if ci.ssl:
				out['ssl'] = True
			out['ttl'] = CONNECTION_TTL
			stats_sock.send('conn ' + tnetstring.dumps(out))

			if ci.sid:
				refresh_sids.append(ci.sid)

		for i in send_activity:
			out = dict()
			out['from'] = instance_id
			if i[0]:
				out['route'] = i[0]
			out['count'] = i[1]
			stats_sock.send('activity ' + tnetstring.dumps(out))

		if refresh_sids and state_rpc:
			try:
				sid_last_ids = dict()
				for sid in refresh_sids:
					sid_last_ids[sid] = dict()
				session_update_many(state_rpc, sid_last_ids)
			except:
				logger.debug("couldn't update sessions")

		time.sleep(1)

def proxy_stats_worker():
	in_sock = ctx.socket(zmq.SUB)
	in_sock.setsockopt(zmq.SUBSCRIBE, '')
	in_sock.connect(proxy_stats_spec)

	stats_sock = ctx.socket(zmq.PUSH)
	stats_sock.linger = 0
	stats_sock.connect('inproc://stats_in')

	if state_spec:
		state_rpc = rpc.RpcClient(['inproc://state'], context=ctx)
	else:
		state_rpc = None

	while True:
		m_raw = in_sock.recv()
		at = m_raw.find(' ')
		mtype = m_raw[:at]
		m = tnetstring.loads(m_raw[at + 1:])
		#logger.debug("IN proxy stats: %s %s" % (mtype, m))
		if mtype == 'activity':
			route = m.get('route')
			if not route:
				route = ''
			count = m['count']
			stats_lock.acquire()
			if route in stats_activity:
				stats_activity[route] += count
			else:
				stats_activity[route] = count
			stats_lock.release()
		elif mtype == 'conn':
			# get sid
			sid = None
			lock.acquire()
			if m.get('type') == 'ws':
				s = ws_sessions.get(m['id'])
				if s is not None:
					sid = s.sid
			lock.release()

			# relay
			m['from'] = instance_id
			stats_sock.send(mtype + ' ' + tnetstring.dumps(m))

			# update session
			if sid and not m.get('unavailable') and state_rpc:
				try:
					session_update_many(state_rpc, {sid: {}})
				except:
					logger.debug("couldn't update session")

def stats_worker(c):
	in_sock = ctx.socket(zmq.PULL)
	in_sock.bind('inproc://stats_in')

	if stats_spec:
		out_sock = ctx.socket(zmq.PUB)
		out_sock.linger = 0
		bind_spec(out_sock, stats_spec)
	else:
		out_sock = None

	if push_in_sub_spec:
		subs_modes_by_channel = dict() # key=channel, value={mode: sub}
		subs_by_exp = SortedDict() # key=expire_time, value=set(sub)
		sub_sock = ctx.socket(zmq.PUSH)
		sub_sock.linger = 0
		sub_sock.bind('inproc://sub_cmd_in')
	else:
		sub_sock = None

	c.acquire()
	c.notify()
	c.release()

	while True:
		try:
			m_raw = in_sock.recv()
			at = m_raw.find(' ')
			mtype = m_raw[:at]
			mdata = m_raw[at + 1:]

			now = int(time.time() * 1000)

			if sub_sock and mtype == 'sub':
				m = tnetstring.loads(mdata)
				mode = m['mode']
				channel = m['channel']
				here = not m.get('unavailable', False)

				if here:
					ttl = m['ttl']
					notify = False
					subs_modes = subs_modes_by_channel.get(channel)
					if subs_modes is None:
						subs_modes = {}
						subs_modes_by_channel[channel] = subs_modes
						notify = True
					sub = subs_modes.get(mode)
					if sub is not None:
						subs_exp = subs_by_exp[sub.expire_time]
						subs_exp.remove(sub)
						if len(subs_exp) == 0:
							del subs_by_exp[sub.expire_time]
					else:
						sub = StatsSubscription()
						sub.mode = mode
						sub.channel = channel
						subs_modes[mode] = sub
					sub.expire_time = now + (ttl * 1000)
					subs_exp = subs_by_exp.get(sub.expire_time)
					if subs_exp is None:
						subs_exp = set()
						subs_by_exp[sub.expire_time] = subs_exp
					subs_exp.add(sub)
					if notify:
						sm = {'channel': channel}
						sm['type'] = 'subscribe'
						sub_sock.send(tnetstring.dumps(sm))
				else:
					subs_modes = subs_modes_by_channel.get(channel)
					if subs_modes is not None:
						sub = subs_modes.get(mode)
						if sub is not None:
							subs_exp = subs_by_exp[sub.expire_time]
							subs_exp.remove(sub)
							if len(subs_exp) == 0:
								del subs_by_exp[sub.expire_time]
							del subs_modes[mode]
							if len(subs_modes) == 0:
								del subs_modes_by_channel[channel]
								sm = {'channel': channel}
								sm['type'] = 'unsubscribe'
								sub_sock.send(tnetstring.dumps(sm))

			if out_sock:
				m_raw = mtype + ' T' + mdata
				logger.debug('OUT stats: %s' % m_raw)
				out_sock.send(m_raw)

			if sub_sock:
				while len(subs_by_exp) > 0:
					next_exp = iter(subs_by_exp).next()
					if next_exp > now:
						break

					subs_exp = subs_by_exp[next_exp]
					del subs_by_exp[next_exp]
					for sub in subs_exp:
						logger.debug('stats_worker: expiring %s %s' % (sub.mode, sub.channel))
						subs_modes = subs_modes_by_channel.get(sub.channel)
						del subs_modes[sub.mode]
						if len(subs_modes) == 0:
							del subs_modes_by_channel[sub.channel]
							sm = {'channel': sub.channel}
							sm['type'] = 'unsubscribe'
							sub_sock.send(tnetstring.dumps(sm))
		except zmq.ContextTerminated:
			raise
		except:
			logger.exception('failed')
*/

/*
def state_internal_handler(method, args, data):
	state_client = data.get('state_client')
	return state_client.call(method, args, timeout=1000)

def state_internal_worker(c):
	data = dict()
	if state_spec:
		data['state_client'] = rpc.RpcClient([state_spec], bind=True, context=ctx, ipc_file_mode=ipc_file_mode)
	state_internal_server = rpc.RpcServer('inproc://state', context=ctx)
	c.acquire()
	c.notify()
	c.release()
	state_internal_server.run(state_internal_handler, data)

if state_spec:
	# we use a condition here to ensure the inproc bind succeeds before progressing
	c = threading.Condition()
	c.acquire()
	state_internal_thread = threading.Thread(target=state_internal_worker, args=(c,))
	state_internal_thread.daemon = True
	state_internal_thread.start()
	c.wait()
	c.release()
*/
