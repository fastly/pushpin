/*
 * Copyright (C) 2006 Justin Karneges
 * Copyright (C) 2017 Fanout, Inc.
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

#include "processquit.h"

#ifndef NO_IRISNET
# include "irisnetglobal_p.h"
#endif

#ifdef QT_GUI_LIB
# include <QApplication>
#endif

#ifdef Q_OS_WIN
# include <windows.h>
#endif

#ifdef Q_OS_UNIX
# include <signal.h>
# include <unistd.h>
#endif

namespace {

// safeobj stuff, from qca

void releaseAndDeleteLater(QObject *owner, QObject *obj)
{
	obj->disconnect(owner);
	obj->setParent(0);
	obj->deleteLater();
}

class SafeSocketNotifier : public QObject
{
	Q_OBJECT
public:
	SafeSocketNotifier(int socket, QSocketNotifier::Type type,
		QObject *parent = 0) :
		QObject(parent)
	{
		sn = new QSocketNotifier(socket, type, this);
		connect(sn, SIGNAL(activated(int)), SIGNAL(activated(int)));
	}

	~SafeSocketNotifier()
	{
		sn->setEnabled(false);
		releaseAndDeleteLater(this, sn);
	}

	bool isEnabled() const             { return sn->isEnabled(); }
	int socket() const                 { return sn->socket(); }
	QSocketNotifier::Type type() const { return sn->type(); }

public slots:
	void setEnabled(bool enable)       { sn->setEnabled(enable); }

signals:
	void activated(int socket);

private:
	QSocketNotifier *sn;
};

}

#ifndef NO_IRISNET
namespace XMPP {
#endif

Q_GLOBAL_STATIC(QMutex, pq_mutex)
static ProcessQuit *g_pq = 0;

inline bool is_gui_app()
{
#ifdef QT_GUI_LIB
	return (QApplication::type() != QApplication::Tty);
#else
	return false;
#endif
}

class ProcessQuit::Private : public QObject
{
	Q_OBJECT
public:
	ProcessQuit *q;

	bool done;
#ifdef Q_OS_WIN
	bool use_handler;
#endif
#ifdef Q_OS_UNIX
	int sig_pipe[2];
	SafeSocketNotifier *sig_notifier;
#endif

	Private(ProcessQuit *_q) : QObject(_q), q(_q)
	{
		done = false;
#ifdef Q_OS_WIN
		use_handler = !is_gui_app();
		if(use_handler)
			SetConsoleCtrlHandler((PHANDLER_ROUTINE)winHandler, TRUE);
#endif
#ifdef Q_OS_UNIX
		if(pipe(sig_pipe) == -1)
		{
			// no support then
			return;
		}

		sig_notifier = new SafeSocketNotifier(sig_pipe[0], QSocketNotifier::Read, this);
		connect(sig_notifier, SIGNAL(activated(int)), SLOT(sig_activated(int)));
		unixWatchAdd(SIGINT);
		unixWatchAdd(SIGHUP);
		unixWatchAdd(SIGTERM);
#endif
	}

	~Private()
	{
#ifdef Q_OS_WIN
		if(use_handler)
			SetConsoleCtrlHandler((PHANDLER_ROUTINE)winHandler, FALSE);
#endif
#ifdef Q_OS_UNIX
		unixWatchRemove(SIGINT);
		unixWatchRemove(SIGHUP);
		unixWatchRemove(SIGTERM);
		delete sig_notifier;
		close(sig_pipe[0]);
		close(sig_pipe[1]);
#endif
	}

#ifdef Q_OS_WIN
	static BOOL winHandler(DWORD ctrlType)
	{
		Q_UNUSED(ctrlType);
		QMetaObject::invokeMethod(g_pq->d, "ctrl_ready", Qt::QueuedConnection);
		return TRUE;
	}
#endif

#ifdef Q_OS_UNIX
	static void unixHandler(int sig)
	{
		Q_UNUSED(sig);
		unsigned char c = 0;
		if(sig == SIGHUP)
			c = 1;
		if(::write(g_pq->d->sig_pipe[1], &c, 1) == -1)
		{
			// TODO: error handling?
			return;
		}
	}

	void unixWatchAdd(int sig)
	{
		struct sigaction sa;
		sigaction(sig, NULL, &sa);
		// if the signal is ignored, don't take it over.  this is
		//   recommended by the glibc manual
		if(sa.sa_handler == SIG_IGN)
			return;
		sigemptyset(&(sa.sa_mask));
		sa.sa_flags = 0;
		sa.sa_handler = unixHandler;
		sigaction(sig, &sa, 0);
	}

	void unixWatchRemove(int sig)
	{
		struct sigaction sa;
		sigaction(sig, NULL, &sa);
		// ignored means we skipped it earlier, so we should
		//   skip it again
		if(sa.sa_handler == SIG_IGN)
			return;
		sigemptyset(&(sa.sa_mask));
		sa.sa_flags = 0;
		sa.sa_handler = SIG_DFL;
		sigaction(sig, &sa, 0);
	}
#endif

public slots:
	void ctrl_ready()
	{
#ifdef Q_OS_WIN
		do_emit();
#endif
	}

	void sig_activated(int)
	{
#ifdef Q_OS_UNIX
		unsigned char c;
		if(::read(sig_pipe[0], &c, 1) == -1)
		{
			// TODO: error handling?
			return;
		}

		if(c == 1) // SIGHUP
		{
			emit q->hup();
			return;
		}

		do_emit();
#endif
	}

private:
	void do_emit()
	{
		// only signal once
		if(!done)
		{
			done = true;
			emit q->quit();
		}
	}
};

ProcessQuit::ProcessQuit(QObject *parent)
:QObject(parent)
{
	d = new Private(this);
}

ProcessQuit::~ProcessQuit()
{
	delete d;
}

ProcessQuit *ProcessQuit::instance()
{
	QMutexLocker locker(pq_mutex());
	if(!g_pq)
	{
		g_pq = new ProcessQuit;
		g_pq->moveToThread(QCoreApplication::instance()->thread());
#ifndef NO_IRISNET
		irisNetAddPostRoutine(cleanup);
#endif
	}
	return g_pq;
}

void ProcessQuit::reset()
{
	QMutexLocker locker(pq_mutex());
	if(g_pq)
		g_pq->d->done = false;
}

void ProcessQuit::cleanup()
{
	delete g_pq;
	g_pq = 0;
}

#ifndef NO_IRISNET
}
#endif

#include "processquit.moc"
