/*
 * Copyright (C) 2016 Fanout, Inc.
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

#include "service.h"

#include <sys/types.h>
#include <signal.h>
#include <QProcess>
#include "log.h"

class ServiceProcess : public QProcess
{
	Q_OBJECT

public:
	ServiceProcess(QObject *parent = 0) :
		QProcess(parent)
	{
	}

	// reimplemented
	virtual void setupChildProcess()
	{
		signal(SIGINT, SIG_IGN);
	}
};

class Service::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		NotStarted,
		Starting,
		Started,
		Stopping,
		Stopped
	};

	Service *q;
	State state;
	QProcess *proc;
	bool terminateAfterStarted;

	Private(Service *_q) :
		QObject(_q),
		q(_q),
		state(NotStarted),
		proc(0),
		terminateAfterStarted(false)
	{
	}

	~Private()
	{
		if(proc)
		{
			proc->disconnect(this);
			proc->setParent(0);
			proc->deleteLater();
		}
	}

	void start()
	{
		proc = new ServiceProcess(this);

		connect(proc, &QProcess::started, this, &Service::Private::proc_started);
		connect(proc, &QProcess::readyReadStandardOutput, this, &Service::Private::proc_readyRead);
		connect(proc, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this, &Service::Private::proc_finished);
		connect(proc, static_cast<void(QProcess::*)(QProcess::ProcessError)>(&QProcess::error), this, &Service::Private::proc_error);

		proc->setProcessChannelMode(QProcess::MergedChannels);
		proc->setReadChannel(QProcess::StandardOutput);

		state = Starting;

		QStringList args = q->arguments();
		proc->start(args[0], args.mid(1));
	}

	void stop()
	{
		if(state == Starting)
		{
			terminateAfterStarted = true;
		}
		else if(state == Started)
		{
			state = Stopping;
			proc->terminate();
		}
	}

private slots:
	void proc_started()
	{
		state = Started;
		emit q->started();

		if(terminateAfterStarted)
		{
			state = Stopping;
			proc->terminate();
		}
	}

	void proc_readyRead()
	{
		while(proc->canReadLine())
		{
			QByteArray line = proc->readLine();
			if(!line.isEmpty() && line[line.length() - 1] == '\n')
				line.truncate(line.length() - 1);

			emit q->logLine(QString::fromLocal8Bit(line));
		}
	}

	void proc_finished(int exitCode, QProcess::ExitStatus exitStatus)
	{
		if(state != Stopping)
		{
			state = Stopped;
			emit q->error("Exited unexpectedly");
			return;
		}

		state = Stopped;

		if(exitStatus == QProcess::CrashExit)
		{
			emit q->error("Crashed");
			return;
		}

		if(exitCode != 0 && exitCode != -15)
		{
			emit q->error("Unexpected return code: " + QString::number(exitCode));
			return;
		}

		emit q->stopped();
	}

	void proc_error(QProcess::ProcessError error)
	{
		Q_UNUSED(error);

		state = Stopped;

		emit q->error("Error running: " + proc->program());
	}
};

Service::Service(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

Service::~Service()
{
	delete d;
}

bool Service::acceptSighup() const
{
	return false;
}

bool Service::isStarted() const
{
	return (d->state != Private::NotStarted && d->state != Private::Starting);
}

bool Service::preStart()
{
	// by default do nothing
	return true;
}

void Service::start()
{
	if(!preStart())
	{
		QString str = "Failure preparing to start";
		QMetaObject::invokeMethod(this, "error", Qt::QueuedConnection, Q_ARG(QString, str));
		return;
	}

	d->start();
}

void Service::postStart()
{
	// by default do nothing
}

void Service::stop()
{
	d->stop();
}

void Service::postStop()
{
	// by default do nothing
}

void Service::sendSighup()
{
	if(d->proc)
		::kill(d->proc->pid(), SIGHUP);
}

#include "service.moc"
