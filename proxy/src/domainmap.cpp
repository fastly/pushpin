#include "domainmap.h"

#include <stdio.h>
#include <QStringList>
#include <QHash>
#include <QTimer>
#include <QThread>
#include <QMutex>
#include <QFile>
#include <QTextStream>
#include <QFileSystemWatcher>

// TODO: fix races around startup and shutdown

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
			// TODO: log warning
			return;
		}

		QHash< QString, QList<Target> > newmap;

		QTextStream ts(&file);
		while(!ts.atEnd())
		{
			QString line = ts.readLine();

			QStringList parts = line.split(' ', QString::SkipEmptyParts);
			if(parts.count() < 2)
			{
				// TODO: log warning
				continue;
			}

			if(newmap.contains(parts[0]))
			{
				// TODO: log warning
				continue;
			}

			bool ok = true;
			QList<Target> targets;
			for(int n = 1; n < parts.count(); ++n)
			{
				int at = parts[n].lastIndexOf(':');
				if(at == -1)
				{
					// TODO: log warning
					ok = false;
					break;
				}

				QString sport = parts[n].mid(at + 1);
				int port = sport.toInt(&ok);
				if(!ok || port < 1 || port > 65535)
				{
					// TODO: log warning
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

		printf("domain map:\n");
		QHashIterator< QString, QList<Target> > it(newmap);
		while(it.hasNext())
		{
			it.next();

			QStringList tstr;
			foreach(const Target &t, it.value())
				tstr += t.first + ';' + QString::number(t.second);

			printf("  %s: %s\n", qPrintable(it.key()), qPrintable(tstr.join(" ")));
		}

		m.lock();
		map = newmap;
		m.unlock();
	}

public slots:
	void start()
	{
		QFileSystemWatcher *watcher = new QFileSystemWatcher(this);
		connect(watcher, SIGNAL(fileChanged(const QString &)), SLOT(fileChanged(const QString &)));
		watcher->addPath(fileName);

		reload();
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
		printf("reloading domains\n");
		reload();
	}
};

class DomainMap::Thread : public QThread
{
	Q_OBJECT

public:
	QString fileName;
	Worker *worker;

	~Thread()
	{
		quit();
		wait();
	}

	virtual void run()
	{
		worker = new Worker;
		worker->fileName = fileName;
		QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);
		exec();
		delete worker;
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
