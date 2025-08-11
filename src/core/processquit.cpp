/*
 * Copyright (C) 2006 Justin Karneges
 * Copyright (C) 2017 Fanout, Inc.
 * Copyright (C) 2025 Fastly, Inc.
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

#include <signal.h>
#include <unistd.h>
#include <QGlobalStatic>
#include <QMutex>
#include "socketnotifier.h"

Q_GLOBAL_STATIC(QMutex, pq_mutex)
static ProcessQuit *g_pq = nullptr;

class ProcessQuit::Private
{
public:
	ProcessQuit *q;
	Connection activatedConnection;

	bool done;
	int sig_pipe[2];
	std::unique_ptr<SocketNotifier> sig_notifier;

	Private(ProcessQuit *_q) :
		q(_q)
	{
		done = false;

		if(pipe(sig_pipe) == -1)
		{
			// no support then
			return;
		}

		sig_notifier = std::make_unique<SocketNotifier>(sig_pipe[0], SocketNotifier::Read);
		activatedConnection = sig_notifier->activated.connect(boost::bind(&Private::sig_activated, this, boost::placeholders::_1));
		sig_notifier->clearReadiness(SocketNotifier::Read);
		unixWatchAdd(SIGINT);
		unixWatchAdd(SIGHUP);
		unixWatchAdd(SIGTERM);
	}

	~Private()
	{
		unixWatchRemove(SIGINT);
		unixWatchRemove(SIGHUP);
		unixWatchRemove(SIGTERM);
		activatedConnection.disconnect();
		sig_notifier.reset();
		close(sig_pipe[0]);
		close(sig_pipe[1]);
	}

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

	void sig_activated(int)
	{
		sig_notifier->clearReadiness(SocketNotifier::Read);

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

ProcessQuit::ProcessQuit()
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
		g_pq = new ProcessQuit;
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
	g_pq = nullptr;
}
