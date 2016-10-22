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
#include <QCommandLineParser>
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

enum CommandLineParseResult
{
	CommandLineOk,
	CommandLineError,
	CommandLineVersionRequested,
	CommandLineHelpRequested
};

class ArgsData
{
public:
	QString configFile;
	QString logFile;
	int logLevel;
	QString ipcPrefix;
	int portOffset;

	ArgsData() :
		logLevel(-1),
		portOffset(-1)
	{
	}
};

static CommandLineParseResult parseCommandLine(QCommandLineParser *parser, ArgsData *args, QString *errorMessage)
{
	parser->setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
	const QCommandLineOption configFileOption("config", "Config file.", "file");
	parser->addOption(configFileOption);
	const QCommandLineOption logFileOption("logfile", "File to log to.", "file");
	parser->addOption(logFileOption);
	const QCommandLineOption logLevelOption("loglevel", "Log level (default: 2).", "x");
	parser->addOption(logLevelOption);
	const QCommandLineOption verboseOption("verbose", "Verbose output. Same as --loglevel=3.");
	parser->addOption(verboseOption);
	const QCommandLineOption ipcPrefixOption("ipc-prefix", "Override ipc_prefix config option.", "prefix");
	parser->addOption(ipcPrefixOption);
	const QCommandLineOption portOffsetOption("port-offset", "Override port_offset config option.", "offset");
	parser->addOption(portOffsetOption);
	const QCommandLineOption helpOption = parser->addHelpOption();
	const QCommandLineOption versionOption = parser->addVersionOption();

	if(!parser->parse(QCoreApplication::arguments()))
	{
		*errorMessage = parser->errorText();
		return CommandLineError;
	}

	if(parser->isSet(versionOption))
		return CommandLineVersionRequested;

	if(parser->isSet(helpOption))
		return CommandLineHelpRequested;

	if(parser->isSet(configFileOption))
		args->configFile = parser->value(configFileOption);

	if(parser->isSet(logFileOption))
		args->logFile = parser->value(logFileOption);

	if(parser->isSet(logLevelOption))
	{
		bool ok;
		int x = parser->value(logLevelOption).toInt(&ok);
		if(!ok || x < 0)
		{
			*errorMessage = "error: loglevel must be greater than or equal to 0";
			return CommandLineError;
		}

		args->logLevel = x;
	}

	if(parser->isSet(verboseOption))
		args->logLevel = 3;

	if(parser->isSet(ipcPrefixOption))
		args->ipcPrefix = parser->value(ipcPrefixOption);

	if(parser->isSet(portOffsetOption))
	{
		bool ok;
		int x = parser->value(portOffsetOption).toInt(&ok);
		if(!ok || x < 0)
		{
			*errorMessage = "error: port-offset must be greater than or equal to 0";
			return CommandLineError;
		}

		args->portOffset = x;
	}

	return CommandLineOk;
}

class App::Private : public QObject
{
	Q_OBJECT

public:
	App *q;
	ArgsData args;
	Engine *engine;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		engine(0)
	{
		connect(ProcessQuit::instance(), &ProcessQuit::quit, this, &Private::doQuit);
		connect(ProcessQuit::instance(), &ProcessQuit::hup, this, &Private::reload);
	}

	void start()
	{
		QCoreApplication::setApplicationName("pushpin-handler");
		QCoreApplication::setApplicationVersion(VERSION);

		QCommandLineParser parser;
		parser.setApplicationDescription("Pushpin handler component.");

		QString errorMessage;
		switch(parseCommandLine(&parser, &args, &errorMessage))
		{
			case CommandLineOk:
				break;
			case CommandLineError:
				fprintf(stderr, "%s\n\n%s", qPrintable(errorMessage), qPrintable(parser.helpText()));
				emit q->quit(1);
				return;
			case CommandLineVersionRequested:
				printf("%s %s\n", qPrintable(QCoreApplication::applicationName()),
					qPrintable(QCoreApplication::applicationVersion()));
				emit q->quit(0);
				return;
			case CommandLineHelpRequested:
				parser.showHelp();
				Q_UNREACHABLE();
		}

		if(args.logLevel != -1)
			log_setOutputLevel(args.logLevel);
		else
			log_setOutputLevel(LOG_LEVEL_INFO);

		if(!args.logFile.isEmpty())
		{
			if(!log_setFile(args.logFile))
			{
				log_error("failed to open log file: %s", qPrintable(args.logFile));
				emit q->quit(1);
				return;
			}
		}

		log_info("starting...");

		QString configFile = args.configFile;
		if(configFile.isEmpty())
			configFile = QDir(CONFIGDIR).filePath("pushpin.conf");

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

		if(!args.ipcPrefix.isEmpty())
			settings.setIpcPrefix(args.ipcPrefix);

		if(args.portOffset != -1)
			settings.setPortOffset(args.portOffset);

		QStringList m2a_in_stream_specs = settings.value("handler/m2a_in_stream_specs").toStringList();
		trimlist(&m2a_in_stream_specs);
		QStringList m2a_out_specs = settings.value("handler/m2a_out_specs").toStringList();
		trimlist(&m2a_out_specs);
		QStringList intreq_out_specs = settings.value("handler/proxy_intreq_out_specs").toStringList();
		trimlist(&intreq_out_specs);
		QStringList intreq_out_stream_specs = settings.value("handler/proxy_intreq_out_stream_specs").toStringList();
		trimlist(&intreq_out_stream_specs);
		QStringList intreq_in_specs = settings.value("handler/proxy_intreq_in_specs").toStringList();
		trimlist(&intreq_in_specs);
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
		int push_in_http_port = settings.adjustedPort("handler/push_in_http_port");
		bool ok;
		int ipcFileMode = settings.value("handler/ipc_file_mode", -1).toString().toInt(&ok, 8);
		bool shareAll = settings.value("handler/share_all").toBool();
		int messageRate = settings.value("handler/message_rate", -1).toInt();
		int messageHwm = settings.value("handler/message_hwm", -1).toInt();

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
		config.clientOutSpecs = intreq_out_specs;
		config.clientOutStreamSpecs = intreq_out_stream_specs;
		config.clientInSpecs = intreq_in_specs;
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
		config.messageRate = messageRate;
		config.messageHwm = messageHwm;

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
