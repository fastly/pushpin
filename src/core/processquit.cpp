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

#include "socketnotifier.h"

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
	Connection activatedConnection;

	bool done;
#ifdef Q_OS_WIN
	bool use_handler;
#endif
#ifdef Q_OS_UNIX
	int sig_pipe[2];
	std::unique_ptr<SocketNotifier> sig_notifier;
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

		sig_notifier = std::make_unique<SocketNotifier>(sig_pipe[0], SocketNotifier::Read);
		activatedConnection = sig_notifier->activated.connect(boost::bind(&Private::sig_activated, this, boost::placeholders::_1));
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
		activatedConnection.disconnect();
		sig_notifier.reset();
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
			q->hup();
			return;
		}

		do_emit();
#endif
	}

public slots:
	void ctrl_ready()
	{
#ifdef Q_OS_WIN
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
			q->quit();
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
