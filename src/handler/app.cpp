/*
 * Copyright (C) 2015-2016 Fanout, Inc.
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

#include "app.h"

#include <assert.h>
#include <QCoreApplication>
#include <QStringList>
#include <QFile>
#include <QFileInfo>
#include "processquit.h"
#include "log.h"
#include "settings.h"
#include "engine.h"
#include "config.h"

static void trimlist(QStringList *list)
{
	for(int n = 0; n < list->count(); ++n)
	{
		if((*list)[n].isEmpty())
		{
			list->removeAt(n);
			--n; // adjust position
		}
	}
}

class App::Private : public QObject
{
	Q_OBJECT

public:
	App *q;
	Engine *engine;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		engine(0)
	{
		connect(ProcessQuit::instance(), SIGNAL(quit()), SLOT(doQuit()));
		connect(ProcessQuit::instance(), SIGNAL(hup()), SLOT(reload()));
	}

	void start()
	{
		QStringList args = QCoreApplication::instance()->arguments();
		args.removeFirst();

		// options
		QHash<QString, QString> options;
		for(int n = 0; n < args.count(); ++n)
		{
			if(args[n] == "--")
			{
				break;
			}
			else if(args[n].startsWith("--"))
			{
				QString opt = args[n].mid(2);
				QString var, val;

				int at = opt.indexOf("=");
				if(at != -1)
				{
					var = opt.mid(0, at);
					val = opt.mid(at + 1);
				}
				else
					var = opt;

				options[var] = val;

				args.removeAt(n);
				--n; // adjust position
			}
		}

		if(options.contains("version"))
		{
			printf("pushpin-handler %s\n", VERSION);
			emit q->quit();
			return;
		}

		if(options.contains("verbose"))
			log_setOutputLevel(LOG_LEVEL_DEBUG);
		else
			log_setOutputLevel(LOG_LEVEL_INFO);

		QString logFile = options.value("logfile");
		if(!logFile.isEmpty())
		{
			if(!log_setFile(logFile))
			{
				log_error("failed to open log file: %s", qPrintable(logFile));
				emit q->quit();
				return;
			}
		}

		log_info("starting...");

		QString configFile = options.value("config");
		if(configFile.isEmpty())
			configFile = "/etc/pushpin/pushpin.conf";

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				log_error("failed to open %s, and --config not passed", qPrintable(configFile));
				emit q->quit();
				return;
			}
		}

		Settings settings(configFile);

		QStringList m2a_in_stream_specs = settings.value("handler/m2a_in_stream_specs").toStringList();
		trimlist(&m2a_in_stream_specs);
		QStringList m2a_out_specs = settings.value("handler/m2a_out_specs").toStringList();
		trimlist(&m2a_out_specs);
		QString proxy_inspect_spec = settings.value("handler/proxy_inspect_spec").toString();
		QString proxy_accept_spec = settings.value("handler/proxy_accept_spec").toString();
		QString proxy_retry_out_spec = settings.value("handler/proxy_retry_out_spec").toString();
		QString ws_control_in_spec = settings.value("handler/proxy_ws_control_in_spec").toString();
		QString ws_control_out_spec = settings.value("handler/proxy_ws_control_out_spec").toString();
		QString stats_spec = settings.value("handler/stats_spec").toString();
		QString command_spec = settings.value("handler/command_spec").toString();
		QString state_spec = settings.value("handler/state_spec").toString();
		QString proxy_stats_spec = settings.value("handler/proxy_stats_spec").toString();
		QString proxy_command_spec = settings.value("handler/proxy_command_spec").toString();
		QString push_in_spec = settings.value("handler/push_in_spec").toString();
		QString push_in_sub_spec = settings.value("handler/push_in_sub_spec").toString();
		QString push_in_http_addr = settings.value("handler/push_in_http_addr").toString();
		int push_in_http_port = settings.value("handler/push_in_http_port").toInt();
		bool ok;
		int ipcFileMode = settings.value("handler/ipc_file_mode", -1).toString().toInt(&ok, 8);
		bool shareAll = settings.value("handler/share_all").toBool();

		if(m2a_in_stream_specs.isEmpty() || m2a_out_specs.isEmpty())
		{
			log_error("must set m2a_in_stream_specs and m2a_out_specs");
			emit q->quit();
			return;
		}

		if(proxy_inspect_spec.isEmpty() || proxy_accept_spec.isEmpty() || proxy_retry_out_spec.isEmpty())
		{
			log_error("must set proxy_inspect_spec, proxy_accept_spec, and proxy_retry_out_spec");
			emit q->quit();
			return;
		}

		Engine::Configuration config;
		config.appVersion = VERSION;
		config.instanceId = "pushpin-handler_" + QByteArray::number(QCoreApplication::applicationPid());
		config.serverInStreamSpecs = m2a_in_stream_specs;
		config.serverOutSpecs = m2a_out_specs;
		config.inspectSpec = proxy_inspect_spec;
		config.acceptSpec = proxy_accept_spec;
		config.retryOutSpec = proxy_retry_out_spec;
		config.wsControlInSpec = ws_control_in_spec;
		config.wsControlOutSpec = ws_control_out_spec;
		config.statsSpec = stats_spec;
		config.commandSpec = command_spec;
		config.stateSpec = state_spec;
		config.proxyStatsSpec = proxy_stats_spec;
		config.proxyCommandSpec = proxy_command_spec;
		config.pushInSpec = push_in_spec;
		config.pushInSubSpec = push_in_sub_spec;
		config.pushInHttpAddr = QHostAddress(push_in_http_addr);
		config.pushInHttpPort = push_in_http_port;
		config.ipcFileMode = ipcFileMode;
		config.shareAll = shareAll;

		engine = new Engine(this);
		if(!engine->start(config))
		{
			emit q->quit();
			return;
		}

		log_info("started");
	}

private slots:
	void reload()
	{
		log_info("reloading");
		log_rotate();
		engine->reload();
	}

	void doQuit()
	{
		log_info("stopping...");

		// remove the handler, so if we get another signal then we crash out
		ProcessQuit::cleanup();

		delete engine;
		engine = 0;

		log_info("stopped");
		emit q->quit();
	}
};

App::App(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

App::~App()
{
	delete d;
}

void App::start()
{
	d->start();
}

#include "app.moc"
