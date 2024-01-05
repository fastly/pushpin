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

#ifndef PROCESSQUIT_H
#define PROCESSQUIT_H

#include <boost/signals2.hpp>

using Signal = boost::signals2::signal<void()>;
using SignalInt = boost::signals2::signal<void(int)>;
using Connection = boost::signals2::scoped_connection;

#ifdef NO_IRISNET
# include <QtCore>
# define IRISNET_EXPORT
#else
# include "irisnetglobal.h"
#endif

#ifndef NO_IRISNET
namespace XMPP {
#endif

/**
   \brief Listens for termination requests

   ProcessQuit listens for requests to terminate the application process.  On Unix platforms, these are the signals SIGINT, SIGHUP, and SIGTERM.  On Windows, these are the console control events for Ctrl+C, console window close, and system shutdown.  For Windows GUI programs, ProcessQuit has no effect.

   For GUI programs, ProcessQuit is not a substitute for QSessionManager.  The only safe way to handle termination of a GUI program in the usual way is to use QSessionManager.  However, ProcessQuit does give additional benefit to Unix GUI programs that might be terminated unconventionally, so it can't hurt to support both.

   When a termination request is received, the application should exit gracefully, and generally without user interaction.  Otherwise, it is at risk of being terminated outside of its control.  For example, if a Windows console application does not exit after just a few seconds of attempting to close the console window, Windows will display a prompt to the user asking if the process should be ended immediately.

   Using ProcessQuit is easy, and it usually amounts to a single line:
   \code
myapp.connect(ProcessQuit::instance(), SIGNAL(quit()), SLOT(do_quit()));
   \endcode

   Calling instance() returns a pointer to the global ProcessQuit instance, which will be created if necessary.  The quit() signal is emitted when a request to terminate is received.    The quit() signal is only emitted once, future termination requests are ignored.  Call reset() to allow the quit() signal to be emitted again.
*/
class IRISNET_EXPORT ProcessQuit : public QObject
{
	Q_OBJECT
public:
	/**
	   \brief Returns the global ProcessQuit instance

	   If the global instance does not exist yet, it will be created, and the termination handlers will be installed.

	   \sa cleanup
	*/
	static ProcessQuit *instance();

	/**
	   \brief Allows the quit() signal to be emitted again

	   ProcessQuit only emits the quit() signal once, so that if a user repeatedly presses Ctrl-C or sends SIGTERM, your shutdown slot will not be called multiple times.  This is normally the desired behavior, but if you are ignoring the termination request then you may want to allow future notifications.  Calling this function will allow the quit() signal to be emitted again, if a new termination request arrives.

	   \sa quit
	*/
	static void reset();

	/**
	   \brief Frees all resources used by ProcessQuit

	   This function will free any resources used by ProcessQuit, including the global instance, and the termination handlers will be uninstalled (reverted to default).  Future termination requests will cause the application to exit abruptly.

	   \note You normally do not need to call this function directly.  When IrisNet cleans up, it will be called.

	   \sa instance
	*/
	static void cleanup();

	Signal quit;

	Signal hup;

private:
	class Private;
	friend class Private;
	Private *d;

	ProcessQuit(QObject *parent = 0);
	~ProcessQuit();
};

#ifndef NO_IRISNET
}
#endif

#endif
