/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#include "domainmap.h"

#include <assert.h>
#include <QStringList>
#include <QHash>
#include <QTimer>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QFile>
#include <QTextStream>
#include <QFileSystemWatcher>
#include "log.h"

static QByteArray parse_key(const QString &in)
{
	if(in.startsWith("base64:"))
		return QByteArray::fromBase64(in.mid(7).toUtf8());
	else
		return in.toUtf8();
}

// items are of the format: {value}(,propname=propval,...)
static bool parseItem(const QString &item, QString *_value, QHash<QString, QString> *_props, QString *errmsg)
{
	// read value
	int at = item.indexOf(',');
	QString value;
	if(at != -1)
		value = item.mid(0, at);
	else
		value = item;

	if(value.isEmpty())
	{
		*errmsg = "empty item value";
		return false;
	}

	// read props
	QHash<QString, QString> props;
	int start = at + 1;
	bool done = false;
	while(!done)
	{
		at = item.indexOf(',', start);

		QString attrib;
		if(at != -1)
		{
			attrib = item.mid(start, at - start);
			start = at + 1;
		}
		else
		{
			attrib = item.mid(start);
			done = true;
		}

		at = attrib.indexOf('=');
		QString var, val;
		if(at != -1)
		{
			var = attrib.mid(0, at);
			val = attrib.mid(at + 1);
		}
		else
			var = attrib;

		if(var.isEmpty())
		{
			*errmsg = "empty property name";
			return false;
		}

		if(props.contains(var))
		{
			*errmsg = "duplicate property: " + var;
			return false;
		}

		props[var] = val;
	}

	*_value = value;
	*_props = props;
	return true;
}

class DomainMap::Worker : public QObject
{
	Q_OBJECT

public:
	class Rule
	{
	public:
		int proto; // -1=unspecified, 0=http, 1=websocket
		QByteArray pathBeg;
		int ssl; // -1=unspecified, 0=no, 1=yes

		QByteArray sigIss;
		QByteArray sigKey;
		QByteArray prefix;
		bool origHeaders;
		QString asHost;
		int pathRemove;
		QList<Target> targets;

		Rule() :
			proto(-1),
			ssl(-1),
			origHeaders(false),
			pathRemove(0)
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

			if(other.ssl == -1 && ssl != -1)
				return true;
			else if(other.ssl != -1 && ssl == -1)
				return false;

			if(pathBeg.size() > other.pathBeg.size() && reqPath.startsWith(pathBeg))
				return true;

			return false;
		}
	};

	QMutex m;
	QString fileName;
	QHash< QString, QList<Rule> > map;
	QTimer t;

	Worker() :
		t(this)
	{
		connect(&t, SIGNAL(timeout()), SLOT(doReload()));
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

		QHash< QString, QList<Rule> > newmap;

		QTextStream ts(&file);
		for(int lineNum = 0; !ts.atEnd(); ++lineNum)
		{
			QString line = ts.readLine();

			// strip comments
			int at = line.indexOf('#');
			if(at != -1)
				line.truncate(at);

			line = line.trimmed();
			if(line.isEmpty())
				continue;

			QStringList parts = line.split(' ', QString::SkipEmptyParts);
			if(parts.count() < 2)
			{
				log_warning("%s:%d: must specify rule and at least one target", qPrintable(fileName), lineNum);
				continue;
			}

			QString val;
			QHash<QString, QString> props;
			QString errmsg;
			if(!parseItem(parts[0], &val, &props, &errmsg))
			{
				log_warning("%s:%d: %s", qPrintable(fileName), lineNum, qPrintable(errmsg));
				continue;
			}

			if(val == "*")
				val = QString();

			QString domain = val;

			Rule r;

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
					continue;
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
					continue;
				}
			}

			if(props.contains("path_beg"))
			{
				QString pathBeg = props.value("path_beg");
				if(pathBeg.isEmpty())
				{
					log_warning("%s:%d: path_beg cannot be empty", qPrintable(fileName), lineNum);
					continue;
				}

				r.pathBeg = pathBeg.toUtf8();
			}

			if(props.contains("sig_iss"))
			{
				r.sigIss = props.value("sig_iss").toUtf8();
			}

			if(props.contains("sig_key"))
			{
				r.sigKey = parse_key(props.value("sig_key"));
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

			QList<Rule> *rules = 0;
			if(newmap.contains(domain))
			{
				rules = &newmap[domain];
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
				{
					log_warning("%s:%d skipping duplicate condition", qPrintable(fileName), lineNum);
					continue;
				}
			}

			bool ok = true;
			for(int n = 1; n < parts.count(); ++n)
			{
				if(!parseItem(parts[n], &val, &props, &errmsg))
				{
					log_warning("%s:%d: %s", qPrintable(fileName), lineNum, qPrintable(errmsg));
					ok = false;
					break;
				}

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

				Target target;
				target.connectHost = parts[n].mid(0, at);
				target.connectPort = port;

				if(props.contains("ssl"))
					target.ssl = true;

				if(props.contains("untrusted"))
					target.trusted = false;
				else
					target.trusted = true;

				if(props.contains("insecure"))
					target.insecure = true;

				if(props.contains("host"))
					target.host = props.value("host");

				if(props.contains("sub"))
					target.subChannel = props.value("sub");

				r.targets += target;
			}

			if(!ok)
				continue;

			if(!rules)
			{
				newmap.insert(domain, QList<Rule>());
				rules = &newmap[domain];
			}

			*rules += r;
		}

		log_debug("routes map:");
		QHashIterator< QString, QList<Rule> > it(newmap);
		while(it.hasNext())
		{
			it.next();

			const QString &domain = it.key();
			const QList<Rule> &rules = it.value();
			foreach(const Rule &r, rules)
			{
				QStringList tstr;
				foreach(const Target &t, r.targets)
					tstr += t.connectHost + ';' + QString::number(t.connectPort);

				if(!domain.isEmpty())
					log_debug("  %s: %s", qPrintable(domain), qPrintable(tstr.join(" ")));
				else
					log_debug("  (default): %s", qPrintable(tstr.join(" ")));
			}
		}

		// atomically replace the map
		m.lock();
		map = newmap;
		m.unlock();

		log_info("routes map loaded with %d entries", newmap.count());
	}

signals:
	void started();

public slots:
	void start()
	{
		QFileSystemWatcher *watcher = new QFileSystemWatcher(this);
		connect(watcher, SIGNAL(fileChanged(const QString &)), SLOT(fileChanged(const QString &)));
		watcher->addPath(fileName);

		reload();

		emit started();
	}

	void fileChanged(const QString &path)
	{
		Q_UNUSED(path);

		// inotify tends to give us extra events so let's hang around a
		//   little bit before reloading
		if(!t.isActive())
			t.start(1000);
	}

	void doReload()
	{
		log_info("routes file changed, reloading");
		reload();
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
		connect(worker, SIGNAL(started()), SLOT(worker_started()), Qt::DirectConnection);
		QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);
		exec();
		delete worker;
	}

public slots:
	void worker_started()
	{
		QMutexLocker locker(&m);
		w.wakeOne();
	}
};

class DomainMap::Private : public QObject
{
public:
	Thread *thread;

	Private() :
		thread(0)
	{
	}

	~Private()
	{
		delete thread;
	}

	void start(const QString &fileName)
	{
		thread = new Thread;
		thread->fileName = fileName;
		thread->start();
	}
};

DomainMap::DomainMap(const QString &fileName)
{
	d = new Private;
	d->start(fileName);
}

DomainMap::~DomainMap()
{
	delete d;
}

void DomainMap::reload()
{
	QMetaObject::invokeMethod(d->thread->worker, "fileChanged", Qt::QueuedConnection, Q_ARG(QString, QString()));
}

DomainMap::Entry DomainMap::entry(Protocol proto, bool ssl, const QString &domain, const QByteArray &path) const
{
	QMutexLocker locker(&d->thread->worker->m);

	const QList<Worker::Rule> *rules;
	QString empty("");
	if(d->thread->worker->map.contains(domain))
		rules = &d->thread->worker->map[domain];
	else if(d->thread->worker->map.contains(empty))
		rules = &d->thread->worker->map[empty];
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

	Entry e;
	e.sigIss = best->sigIss;
	e.sigKey = best->sigKey;
	e.prefix = best->prefix;
	e.origHeaders = best->origHeaders;
	e.asHost = best->asHost;
	e.pathRemove = best->pathRemove;
	e.targets = best->targets;

	return e;
}

#include "domainmap.moc"
