/*
 * Copyright (C) 2016-2022 Fanout, Inc.
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

#include "service.h"

#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <QTimer>
#include <QFile>
#include <QProcess>
#include "log.h"

#define STOP_TIMEOUT 4000

static void setupChild()
{
	signal(SIGINT, SIG_IGN);

	// subprocesses hopefully respect SIG_IGN, but are not required
	//   to. in case subprocess might reinstate a SIGINT handler,
	//   detach from process group to ensure ctrl-c in a shell
	//   doesn't cause SIGINT to be sent directly to subprocesses
	setpgid(0, 0);
}

class ServiceProcess : public QProcess
{
	Q_OBJECT

public:
	ServiceProcess(QObject *parent = 0) :
		QProcess(parent)
	{
#if QT_VERSION >= 0x060000
		setChildProcessModifier(setupChild);
#endif
	}

#if QT_VERSION < 0x060000
	// reimplemented
	virtual void setupChildProcess()
	{
		setupChild();
	}
#endif
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
	QString name;
	QString outputFile;
	QString pidFile;
	QProcess *proc;
	bool terminateAfterStarted;
	bool sentKill;
	QTimer *timer;

	Private(Service *_q) :
		QObject(_q),
		q(_q),
		state(NotStarted),
		proc(0),
		terminateAfterStarted(false),
		sentKill(false)
	{
		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, &Private::timer_timeout);

		timer->setSingleShot(true);
	}

	~Private()
	{
		timer->stop();

		if(state == Starting || state == Started || state == Stopping)
		{
			proc->disconnect(this);
			proc->setParent(0);

			if(!sentKill)
			{
				sentKill = true;
				state = Stopping;
				log_warning("%s running while needing to exit, forcing quit", qPrintable(name));
				proc->kill();

				if(!pidFile.isEmpty())
					QFile::remove(pidFile);
			}
			proc->waitForFinished();
		}

		cleanup();

		timer->disconnect(this);
		timer->setParent(0);
		timer->deleteLater();
	}

	void cleanup()
	{
		timer->stop();

		if(proc)
		{
			proc->disconnect(this);
			proc->setParent(0);
			proc->deleteLater();
			proc = 0;
		}
	}

	void start()
	{
		proc = new ServiceProcess(this);

		connect(proc, &QProcess::started, this, &Private::proc_started);
		connect(proc, &QProcess::readyReadStandardOutput, this, &Private::proc_readyRead);
		connect(proc, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this, &Private::proc_finished);
		connect(proc, static_cast<void(QProcess::*)(QProcess::ProcessError)>(&QProcess::errorOccurred), this, &Private::proc_errorOccurred);

		proc->setProcessChannelMode(QProcess::MergedChannels);
		proc->setReadChannel(QProcess::StandardOutput);

		if(!outputFile.isEmpty())
			proc->setStandardOutputFile(outputFile, QIODevice::Append);

		state = Starting;

		QStringList args = q->arguments();

		log_debug("running: %s", qPrintable(args.join(' ')));

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
			doStop();
		}
	}

private:
	void doStop()
	{
		state = Stopping;
		timer->start(STOP_TIMEOUT);
		proc->terminate();
	}

	bool writePidFile(const QString &file, int pid)
	{
		QFile f(file);
		if(!f.open(QFile::WriteOnly | QFile::Truncate))
			return false;

		if(f.write(QByteArray::number(pid) + '\n') == -1)
			return false;

		return true;
	}

private slots:
	void proc_started()
	{
		if(!pidFile.isEmpty())
		{
			if(!writePidFile(pidFile, proc->processId()))
				log_error("failed to write pid file: %s", qPrintable(pidFile));
		}

		state = Started;
		emit q->started();

		if(terminateAfterStarted)
			doStop();
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
		if(!pidFile.isEmpty())
			QFile::remove(pidFile);

		if(state != Stopping)
		{
			state = Stopped;
			cleanup();
			emit q->error("Exited unexpectedly");
			return;
		}

		state = Stopped;
		cleanup();

		if(exitStatus == QProcess::CrashExit)
		{
			if(sentKill)
				emit q->stopped();
			else
				emit q->error("Exited uncleanly");
			return;
		}

		if(exitCode != 0)
		{
			emit q->error("Unexpected return code: " + QString::number(exitCode));
			return;
		}

		emit q->stopped();
	}

	void proc_errorOccurred(QProcess::ProcessError error)
	{
		if(error == QProcess::FailedToStart)
		{
			QString program = proc->program();
			state = Stopped;
			cleanup();

			emit q->error("Error running: " + program);
		}
		else
		{
			// other errors are followed by finished(), so we don't
			//   need to handle them here
		}
	}

	void timer_timeout()
	{
		if(!sentKill)
		{
			sentKill = true;
			log_warning("%s taking too long, forcing quit", qPrintable(name));
			proc->kill();
		}
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

QString Service::name() const
{
	return d->name;
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

QString Service::formatLogLine(const QString &line) const {
	return line;
}

void Service::sendSighup()
{
	if(d->proc)
		::kill(d->proc->processId(), SIGHUP);
}

void Service::setName(const QString &name)
{
	d->name = name;
}

void Service::setStandardOutputFile(const QString &file)
{
	d->outputFile = file;
}

void Service::setPidFile(const QString &file)
{
	d->pidFile = file;
}

#include "service.moc"
