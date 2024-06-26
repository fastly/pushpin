/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
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

#include "domainmap.h"

#include <assert.h>
#include <QStringList>
#include <QHash>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QFileSystemWatcher>
#include "log.h"
#include "rtimer.h"
#include "routesfile.h"

class DomainMap::Worker : public QObject
{
	Q_OBJECT

public:
	enum AddRuleResult
	{
		AddRuleOk,
		AddRuleNoDomainOrId,
		AddRuleDuplicate,
	};

	class Rule
	{
	public:
		QString domain;

		int proto; // -1=unspecified, 0=http, 1=websocket
		QByteArray pathBeg;
		int ssl; // -1=unspecified, 0=no, 1=yes

		QByteArray id;
		bool explicitId; // if the id was provided by the user
		QByteArray sigIss;
		Jwt::EncodingKey sigKey;
		QByteArray prefix;
		bool origHeaders;
		QString asHost;
		int pathRemove;
		QByteArray pathPrepend;
		bool debug;
		bool autoCrossOrigin;
		JsonpConfig jsonpConfig;
		bool session;
		QByteArray sockJsPath;
		QByteArray sockJsAsPath;
		HttpHeaders headers;
		bool grip;
		QList<Target> targets;

		Rule() :
			proto(-1),
			ssl(-1),
			explicitId(false),
			origHeaders(false),
			pathRemove(0),
			debug(false),
			autoCrossOrigin(false),
			session(false),
			grip(true)
		{
		}

		// checks only the condition, not sig/targets
		bool compare(const Rule &other) const
		{
			return (proto == other.proto && ssl == other.ssl && pathBeg == other.pathBeg);
		}

		inline bool matchProto(Protocol reqProto) const
		{
			return ((proto == 0 && reqProto == Http) || (proto == 1 && reqProto == WebSocket));
		}

		inline bool matchSsl(bool reqSsl) const
		{
			return ((ssl == 0 && !reqSsl) || (ssl == 1 && reqSsl));
		}

		bool isMatch(Protocol reqProto, bool reqSsl, const QByteArray &reqPath) const
		{
			return ((proto == -1 || matchProto(reqProto)) && (ssl == -1 || matchSsl(reqSsl)) && (pathBeg.isEmpty() || reqPath.startsWith(pathBeg)));
		}

		bool isMoreSpecificMatch(const Rule &other, Protocol reqProto, bool reqSsl, const QByteArray &reqPath) const
		{
			// have to at least be a match
			if(!isMatch(reqProto, reqSsl, reqPath))
				return false;

			// now let's see if we're a better match

			if(other.proto == -1 && proto != -1)
				return true;
			else if(other.proto != -1 && proto == -1)
				return false;

			if(pathBeg.size() > other.pathBeg.size())
				return true;

			if(other.ssl == -1 && ssl != -1)
				return true;

			return false;
		}

		QByteArray idFromCondition() const {
			QString domainStr;
			if(!domain.isEmpty())
				domainStr = domain;
			else
				domainStr = "*";

			QString protoStr;
			if(proto == 0)
				protoStr = "http";
			else if(proto == 1)
				protoStr = "ws";
			else
				protoStr = "*";

			QString sslStr;
			if(ssl == 0)
				sslStr = "ssl";
			else if(ssl == 1)
				sslStr = "no-ssl";
			else
				sslStr = "*";

			QString pathBegStr;
			if(!pathBeg.isEmpty())
				pathBegStr = pathBeg;
			else
				pathBegStr = "*";

			return (domainStr + ',' + protoStr + ',' + sslStr + ',' + pathBegStr).toUtf8();
		}

		Entry toEntry() const
		{
			Entry e;
			e.pathBeg = pathBeg;
			e.id = id;
			e.sigIss = sigIss;
			e.sigKey = sigKey;
			e.prefix = prefix;
			e.origHeaders = origHeaders;
			e.asHost = asHost;
			e.pathRemove = pathRemove;
			e.pathPrepend = pathPrepend;
			e.debug = debug;
			e.autoCrossOrigin = autoCrossOrigin;
			e.jsonpConfig = jsonpConfig;
			e.session = session;
			e.sockJsPath = sockJsPath;
			e.sockJsAsPath = sockJsAsPath;
			e.headers = headers;
			e.separateStats = explicitId;
			e.grip = grip;
			e.targets = targets;
			return e;
		}
	};

	QMutex m;
	QString fileName;
	QList<Rule> allRules;
	QHash< QString, QList<Rule> > rulesByDomain;
	QHash<QString, Rule> rulesById;
	RTimer t;
	Connection tConnection;
	QFileSystemWatcher watcher;

	Worker() :
		watcher(this)
	{
		tConnection = t.timeout.connect(boost::bind(&Worker::doReload, this));
		t.setSingleShot(true);
	}

	void reload()
	{
		QFile file(fileName);
		if(!file.open(QFile::ReadOnly))
		{
			log_warning("unable to open routes file: %s", qPrintable(fileName));
			return;
		}

		QDir fileDir = QFileInfo(fileName).absoluteDir();

		QList<Rule> all;
		QHash< QString, QList<Rule> > domainMap;
		QHash<QString, Rule> idMap;

		QTextStream ts(&file);
		for(int lineNum = 1; !ts.atEnd(); ++lineNum)
		{
			QString line = ts.readLine();

			Rule r;
			if(!parseRouteLine(line, fileName, lineNum, fileDir, &r))
			{
				// parseRouteLine will have logged a message if needed
				continue;
			}

			if(r.id.isEmpty())
				r.id = r.idFromCondition();

			AddRuleResult ret = addRule(r, &all, &domainMap, &idMap);
			if(ret != AddRuleOk)
			{
				if(ret == AddRuleNoDomainOrId)
					log_warning("%s:%d condition has no domain or id", qPrintable(fileName), lineNum);
				else // AddRuleDuplicate
					log_warning("%s:%d skipping duplicate condition", qPrintable(fileName), lineNum);

				continue;
			}
		}

		log_debug("routes by domain:");
		QHashIterator< QString, QList<Rule> > it(domainMap);
		while(it.hasNext())
		{
			it.next();

			const QString &domain = it.key();
			const QList<Rule> &rules = it.value();
			foreach(const Rule &r, rules)
			{
				QStringList tstr;
				foreach(const Target &t, r.targets)
				{
					if(t.type == Target::Test)
						tstr += "test";
					else if(t.type == Target::Custom)
						tstr += t.zhttpRoute.baseSpec;
					else // Default
						tstr += t.connectHost + ';' + QString::number(t.connectPort);
				}

				if(!domain.isEmpty())
					log_debug("  %s: %s", qPrintable(domain), qPrintable(tstr.join(" ")));
				else
					log_debug("  (default): %s", qPrintable(tstr.join(" ")));
			}
		}

		// atomically replace the map
		m.lock();
		allRules = all;
		rulesByDomain = domainMap;
		rulesById = idMap;
		m.unlock();

		log_info("routes loaded with %d entries", allRules.count());

		QMetaObject::invokeMethod(this, "doChanged", Qt::QueuedConnection);
	}

	// mutex must be locked when calling this method
	bool addRouteLine(const QString &line)
	{
		Rule r;
		if(!parseRouteLine(line, "<route>", 1, QDir::current(), &r))
			return false;

		if(addRule(r, &allRules, &rulesByDomain, &rulesById) != AddRuleOk)
			return false;

		return true;
	}

	Signal started;
	Signal changed;

public slots:
	void doChanged()
	{
		changed();
	}

	void start()
	{
		if(!fileName.isEmpty())
		{
			connect(&watcher, &QFileSystemWatcher::fileChanged, this, &Worker::fileChanged);
			watcher.addPath(fileName);

			reload();
		}

		started();
	}

	void fileChanged(const QString &path)
	{
		Q_UNUSED(path);

		// inotify tends to give us extra events so let's hang around a
		//   little bit before reloading
		if(!t.isActive())
		{
			log_info("routes file changed, reloading");
			t.start(1000);
		}
	}

	void doReload()
	{
		// in case the file was not changed, but overwritten by a different
		// file, re-arm watcher.
		if(!fileName.isEmpty())
		{
			watcher.addPath(fileName);
		}

		reload();
	}

private:
	static bool parseRouteLine(const QString &line, const QString &fileName, int lineNum, const QDir &fileDir, Rule *rule)
	{
		bool ok;
		QString errmsg;
		QList<RoutesFile::RouteSection> sections = RoutesFile::parseLine(line, &ok, &errmsg);
		if(!ok)
		{
			log_warning("%s:%d: %s", qPrintable(fileName), lineNum, qPrintable(errmsg));
			return false;
		}

		if(sections.isEmpty())
		{
			// nothing. could happen if line is blank or commented out
			return false;
		}

		if(sections.count() < 2)
		{
			log_warning("%s:%d: must specify condition and at least one target", qPrintable(fileName), lineNum);
			return false;
		}

		QString val = sections[0].value;
		QMultiHash<QString, QString> props = sections[0].props;

		Rule r;

		if(val.isEmpty())
			r.domain = QString(); // null means unspecified
		else if(val == "*")
			r.domain = QString(""); // empty means wildcard
		else
			r.domain = val; // non-empty means exact match

		r.jsonpConfig.mode = JsonpConfig::Extended;

		if(props.contains("proto"))
		{
			val = props.value("proto");
			if(val == "http")
				r.proto = 0;
			else if(val == "ws")
				r.proto = 1;
			else
			{
				log_warning("%s:%d: proto must be set to 'http' or 'ws'", qPrintable(fileName), lineNum);
				return false;
			}
		}

		if(props.contains("ssl"))
		{
			val = props.value("ssl");
			if(val == "yes")
				r.ssl = 1;
			else if(val == "no")
				r.ssl = 0;
			else
			{
				log_warning("%s:%d: ssl must be set to 'yes' or 'no'", qPrintable(fileName), lineNum);
				return false;
			}
		}

		if(props.contains("id"))
		{
			r.id = props.value("id").toUtf8();
			r.explicitId = true;
		}

		if(props.contains("path_beg"))
		{
			QString pathBeg = props.value("path_beg");
			if(pathBeg.isEmpty())
			{
				log_warning("%s:%d: path_beg cannot be empty", qPrintable(fileName), lineNum);
				return false;
			}

			r.pathBeg = pathBeg.toUtf8();
		}

		if(props.contains("sig_iss"))
		{
			r.sigIss = props.value("sig_iss").toUtf8();
		}

		if(props.contains("sig_key"))
		{
			r.sigKey = Jwt::EncodingKey::fromConfigString(props.value("sig_key"), fileDir);
		}

		if(props.contains("prefix"))
		{
			r.prefix = props.value("prefix").toUtf8();
		}

		if(props.contains("orig_headers"))
		{
			r.origHeaders = true;
		}

		if(props.contains("as_host"))
		{
			r.asHost = props.value("as_host");
		}

		if(props.contains("path_rem"))
		{
			r.pathRemove = props.value("path_rem").toInt();
		}

		if(props.contains("replace_beg"))
		{
			r.pathRemove = r.pathBeg.length();
			r.pathPrepend = props.value("replace_beg").toUtf8();
		}

		if(props.contains("debug"))
			r.debug = true;

		if(props.contains("aco"))
			r.autoCrossOrigin = true;

		if(props.contains("jsonp_mode"))
		{
			val = props.value("jsonp_mode");
			if(val == "basic")
				r.jsonpConfig.mode = JsonpConfig::Basic;
			else if(val == "extended")
				r.jsonpConfig.mode = JsonpConfig::Extended;
			else
			{
				log_warning("%s:%d: jsonp_mode must be set to 'basic' or 'extended'", qPrintable(fileName), lineNum);
				return false;
			}
		}

		if(props.contains("jsonp_cb"))
			r.jsonpConfig.callbackParam = props.value("jsonp_cb").toUtf8();

		if(props.contains("jsonp_body"))
			r.jsonpConfig.bodyParam = props.value("jsonp_body").toUtf8();

		if(props.contains("jsonp_defcb"))
			r.jsonpConfig.defaultCallback = props.value("jsonp_defcb").toUtf8();

		if(r.jsonpConfig.mode == JsonpConfig::Basic)
			r.jsonpConfig.defaultMethod = "POST";
		else // Extended
			r.jsonpConfig.defaultMethod = "GET";

		if(props.contains("jsonp_defmethod"))
			r.jsonpConfig.defaultMethod = props.value("jsonp_defmethod");

		if(props.contains("session"))
			r.session = true;

		if(props.contains("sockjs"))
			r.sockJsPath = props.value("sockjs").toUtf8();

		if(props.contains("sockjs_as_path"))
			r.sockJsAsPath = props.value("sockjs_as_path").toUtf8();

		if(props.contains("header"))
		{
			foreach(const QString &s, props.values("header"))
			{
				int at = s.indexOf(':');
				if(at < 1)
				{
					log_warning("%s:%d: header must use format 'name:value'", qPrintable(fileName), lineNum);
					return false;
				}

				QByteArray name = s.mid(0, at).toUtf8();
				QByteArray value = s.mid(at + 1).toUtf8();

				// trim left side of value
				int n = 0;
				while(n < value.length() && value[n] == ' ')
				{
					++n;
				}
				if(n > 0)
					value = value.mid(n);

				r.headers += HttpHeader(name, value);
			}
		}

		if(props.contains("no_grip"))
			r.grip = false;

		ok = true;
		for(int n = 1; n < sections.count(); ++n)
		{
			QString val = sections[n].value;
			QMultiHash<QString, QString> props = sections[n].props;

			Target target;

			if(val == "test")
			{
				target.type = Target::Test;
			}
			else if(val.startsWith("zhttp/"))
			{
				target.type = Target::Custom;

				target.zhttpRoute.baseSpec = val.mid(6);
			}
			else if(val.startsWith("zhttpreq/"))
			{
				target.type = Target::Custom;

				target.zhttpRoute.baseSpec = val.mid(9);
				target.zhttpRoute.req = true;
			}
			else
			{
				target.type = Target::Default;

				int at = val.indexOf(':');
				if(at == -1)
				{
					log_warning("%s:%d: target bad format", qPrintable(fileName), lineNum);
					ok = false;
					break;
				}

				QString sport = val.mid(at + 1);
				int port = sport.toInt(&ok);
				if(!ok || port < 1 || port > 65535)
				{
					log_warning("%s:%d: target invalid port", qPrintable(fileName), lineNum);
					ok = false;
					break;
				}

				target.connectHost = val.mid(0, at);
				target.connectPort = port;
			}

			if(props.contains("ssl"))
				target.ssl = true;

			if(props.contains("untrusted"))
				target.trusted = false;
			else
				target.trusted = true;

			if(props.contains("trust_connect_host"))
				target.trustConnectHost = true;

			if(props.contains("insecure"))
				target.insecure = true;

			if(props.contains("host"))
				target.host = props.value("host");

			if(props.contains("sub"))
			{
				foreach(const QString &s, props.values("sub"))
				{
					if(!s.isEmpty())
						target.subscriptions += s;
				}
			}

			if(props.contains("over_http"))
				target.overHttp = true;

			if(props.contains("one_event"))
				target.oneEvent = true;

			if(props.contains("ipc_file_mode"))
			{
				bool ok_;
				int x = props.value("ipc_file_mode").toInt(&ok_, 8);
				if(ok_ && x >= 0)
					target.zhttpRoute.ipcFileMode = x;
			}

			r.targets += target;
		}

		if(!ok)
			return false;

		*rule = r;
		return true;
	}

	static AddRuleResult addRule(const Rule &r, QList<Rule> *all, QHash< QString,QList<Rule> > *domainMap, QHash<QString, Rule> *idMap)
	{
		if(r.domain.isNull() && r.id.isEmpty())
			return AddRuleNoDomainOrId;

		bool addByDomain = false;
		bool addById = false;

		if(!r.domain.isNull())
		{
			if(domainMap->contains(r.domain))
			{
				QList<Rule> *rules = &((*domainMap)[r.domain]);

				bool found = false;
				foreach(const Rule &b, *rules)
				{
					if(b.compare(r))
					{
						found = true;
						break;
					}
				}

				if(found)
					return AddRuleDuplicate;
			}

			addByDomain = true;
		}

		if(!r.id.isEmpty())
		{
			if(!idMap->contains(r.id))
			{
				addById = true;
			}
			else
			{
				// mark the key as unusable
				idMap->insert(r.id, Rule());
			}
		}

		*all += r;

		if(addByDomain)
		{
			if(!domainMap->contains(r.domain))
				domainMap->insert(r.domain, QList<Rule>());

			QList<Rule> *rules = &((*domainMap)[r.domain]);

			*rules += r;
		}

		if(addById)
		{
			idMap->insert(r.id, r);
		}

		return AddRuleOk;
	}
};

class DomainMap::Thread : public QThread
{
	Q_OBJECT

public:
	QString fileName;
	Worker *worker;
	QMutex m;
	QWaitCondition w;

	~Thread()
	{
		quit();
		wait();
	}

	void start()
	{
		QMutexLocker locker(&m);
		QThread::start();
		w.wait(&m);
	}

	virtual void run()
	{
		worker = new Worker;
		worker->fileName = fileName;
		Connection startedConnection = worker->started.connect(boost::bind(&Thread::worker_started, this));
		QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);
		exec();
		startedConnection.disconnect();
		delete worker;
	}

public:
	void worker_started()
	{
		QMutexLocker locker(&m);
		w.wakeOne();
	}
};

class DomainMap::Private : public QObject
{
	Q_OBJECT

public:
	DomainMap *q;
	Thread *thread;
	Connection changedConnection;

	Private(DomainMap *_q) :
		QObject(_q),
		q(_q),
		thread(0)
	{
	}

	~Private()
	{
		changedConnection.disconnect();
		delete thread;
	}

	void start(const QString &fileName = QString())
	{
		thread = new Thread;
		thread->fileName = fileName;
		thread->start();

		// worker guaranteed to exist after starting
		changedConnection = thread->worker->changed.connect(boost::bind(&Private::workerChanged, this));
	}

private:
	// NOTE: must be thread-safe. called from separate thread
	void workerChanged()
	{
		QMetaObject::invokeMethod(this, "doChanged", Qt::QueuedConnection);
	}

private slots:
	void doChanged()
	{
		q->changed();
	}
};

DomainMap::DomainMap(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
	d->start();
}

DomainMap::DomainMap(const QString &fileName, QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
	d->start(fileName);
}

DomainMap::~DomainMap()
{
	delete d;
}

void DomainMap::reload()
{
	QMetaObject::invokeMethod(d->thread->worker, "doReload", Qt::QueuedConnection);
}

bool DomainMap::isIdShared(const QString &id) const
{
	QMutexLocker locker(&d->thread->worker->m);

	if(!d->thread->worker->rulesById.contains(id))
		return false;

	const Worker::Rule *r = &d->thread->worker->rulesById[id];

	return r->id.isEmpty();
}

DomainMap::Entry DomainMap::entry(Protocol proto, bool ssl, const QString &domain, const QByteArray &path) const
{
	QMutexLocker locker(&d->thread->worker->m);

	const QList<Worker::Rule> *rules;
	QString empty("");
	if(d->thread->worker->rulesByDomain.contains(domain))
		rules = &d->thread->worker->rulesByDomain[domain];
	else if(d->thread->worker->rulesByDomain.contains(empty))
		rules = &d->thread->worker->rulesByDomain[empty];
	else
		return Entry();

	const Worker::Rule *best = 0;
	foreach(const Worker::Rule &r, *rules)
	{
		if((!best && r.isMatch(proto, ssl, path)) || (best && r.isMoreSpecificMatch(*best, proto, ssl, path)))
		{
			best = &r;
		}
	}

	if(!best)
		return Entry();

	assert(!best->targets.isEmpty());

	return best->toEntry();
}

DomainMap::Entry DomainMap::entry(const QString &id) const
{
	QMutexLocker locker(&d->thread->worker->m);

	if(!d->thread->worker->rulesById.contains(id))
		return Entry();

	const Worker::Rule *r = &d->thread->worker->rulesById[id];

	// this can happen if there were duplicate route IDs
	if(r->id.isEmpty())
		return Entry();

	return r->toEntry();
}

QList<DomainMap::ZhttpRoute> DomainMap::zhttpRoutes() const
{
	QMutexLocker locker(&d->thread->worker->m);

	QList<ZhttpRoute> out;

	foreach(const Worker::Rule &r, d->thread->worker->allRules)
	{
		foreach(const Target &t, r.targets)
		{
			if(!t.zhttpRoute.isNull() && !out.contains(t.zhttpRoute))
				out += t.zhttpRoute;
		}
	}

	return out;
}

bool DomainMap::addRouteLine(const QString &line)
{
	QMutexLocker locker(&d->thread->worker->m);
	return d->thread->worker->addRouteLine(line);
}

#include "domainmap.moc"
