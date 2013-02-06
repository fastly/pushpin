/*
 * Copyright (C) 2012-2013 Fan Out Networks, Inc.
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

class DomainMap::Worker : public QObject
{
	Q_OBJECT

public:
	QMutex m;
	QString fileName;
	QHash< QString, QList<Target> > map;
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
			log_warning("unable to open domains file: %s", qPrintable(fileName));
			return;
		}

		QHash< QString, QList<Target> > newmap;

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
				log_warning("%s:%d: must specify domain and at least one origin", qPrintable(fileName), lineNum);
				continue;
			}

			if(newmap.contains(parts[0]))
			{
				log_warning("%s:%d skipping duplicate entry", qPrintable(fileName), lineNum);
				continue;
			}

			bool ok = true;
			QList<Target> targets;
			for(int n = 1; n < parts.count(); ++n)
			{
				int at = parts[n].lastIndexOf(':');
				if(at == -1)
				{
					log_warning("%s:%d: origin bad format", qPrintable(fileName), lineNum);
					ok = false;
					break;
				}

				QString sport = parts[n].mid(at + 1);
				int port = sport.toInt(&ok);
				if(!ok || port < 1 || port > 65535)
				{
					log_warning("%s:%d: origin invalid port", qPrintable(fileName), lineNum);
					ok = false;
					break;
				}

				QString host = parts[n].mid(0, at);

				targets += Target(host, port);
			}

			if(!ok)
				continue;

			newmap.insert(parts[0], targets);
		}

		log_debug("domain map:");
		QHashIterator< QString, QList<Target> > it(newmap);
		while(it.hasNext())
		{
			it.next();

			QStringList tstr;
			foreach(const Target &t, it.value())
				tstr += t.first + ';' + QString::number(t.second);

			log_debug("  %s: %s", qPrintable(it.key()), qPrintable(tstr.join(" ")));
		}

		// atomically replace the map
		m.lock();
		map = newmap;
		m.unlock();

		log_info("domain map loaded with %d entries", newmap.count());
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
		log_info("domains file changed, reloading");
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

QList<DomainMap::Target> DomainMap::entry(const QString &domain) const
{
	QMutexLocker locker(&d->thread->worker->m);
	return d->thread->worker->map.value(domain);
}

#include "domainmap.moc"
