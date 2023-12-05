/*
 * Copyright (C) 2015-2022 Fanout, Inc.
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

#include "handlerapp.h"

#include <assert.h>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QStringList>
#include <QFile>
#include <QFileInfo>
#include "processquit.h"
#include "log.h"
#include "settings.h"
#include "handlerengine.h"
#include "config.h"

#define DEFAULT_HTTP_MAX_HEADERS_SIZE 10000
#define DEFAULT_HTTP_MAX_BODY_SIZE 1000000

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

class HandlerApp::Private : public QObject
{
	Q_OBJECT

public:
	HandlerApp *q;
	ArgsData args;
	HandlerEngine *engine;
	Connection quitConnection;
	Connection hupConnection;

	Private(HandlerApp *_q) :
		QObject(_q),
		q(_q),
		engine(0)
	{
		quitConnection = ProcessQuit::instance()->quit.connect(boost::bind(&Private::doQuit, this));
		hupConnection = ProcessQuit::instance()->hup.connect(boost::bind(&Private::reload, this));
	}

	void start()
	{
		QCoreApplication::setApplicationName("pushpin-handler");
		QCoreApplication::setApplicationVersion(Config::get().version);

		QCommandLineParser parser;
		parser.setApplicationDescription("Pushpin handler component.");

		QString errorMessage;
		switch(parseCommandLine(&parser, &args, &errorMessage))
		{
			case CommandLineOk:
				break;
			case CommandLineError:
				fprintf(stderr, "%s\n\n%s", qPrintable(errorMessage), qPrintable(parser.helpText()));
				q->quit(1);
				return;
			case CommandLineVersionRequested:
				printf("%s %s\n", qPrintable(QCoreApplication::applicationName()),
					qPrintable(QCoreApplication::applicationVersion()));
				q->quit(0);
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
				q->quit(1);
				return;
			}
		}

		log_info("starting...");

		QString configFile = args.configFile;
		if(configFile.isEmpty())
			configFile = QDir(Config::get().configDir).filePath("pushpin.conf");

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				log_error("failed to open %s, and --config not passed", qPrintable(configFile));
				q->quit(0);
				return;
			}
		}

		Settings settings(configFile);

		if(!args.ipcPrefix.isEmpty())
			settings.setIpcPrefix(args.ipcPrefix);

		if(args.portOffset != -1)
			settings.setPortOffset(args.portOffset);

		QStringList services = settings.value("runner/services").toStringList();

		QStringList condure_in_stream_specs = settings.value("proxy/condure_in_stream_specs").toStringList();
		trimlist(&condure_in_stream_specs);
		QStringList condure_out_specs = settings.value("proxy/condure_out_specs").toStringList();
		trimlist(&condure_out_specs);
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
		QStringList push_in_sub_specs = settings.value("handler/push_in_sub_specs").toStringList();
		trimlist(&push_in_sub_specs);
		QString push_in_sub_spec = settings.value("handler/push_in_sub_spec").toString();
		if(!push_in_sub_spec.isEmpty())
			push_in_sub_specs += push_in_sub_spec;
		bool push_in_sub_connect = settings.value("handler/push_in_sub_connect").toBool();
		QString push_in_http_addr = settings.value("handler/push_in_http_addr").toString();
		int push_in_http_port = settings.adjustedPort("handler/push_in_http_port");
		int push_in_http_max_headers_size = settings.value("handler/push_in_max_headers_size", DEFAULT_HTTP_MAX_HEADERS_SIZE).toInt();
		int push_in_http_max_body_size = settings.value("handler/push_in_max_body_size", DEFAULT_HTTP_MAX_BODY_SIZE).toInt();
		bool ok;
		int ipcFileMode = settings.value("handler/ipc_file_mode", -1).toString().toInt(&ok, 8);
		bool shareAll = settings.value("handler/share_all").toBool();
		int messageRate = settings.value("handler/message_rate", -1).toInt();
		int messageHwm = settings.value("handler/message_hwm", -1).toInt();
		int messageBlockSize = settings.value("handler/message_block_size", -1).toInt();
		int messageWait = settings.value("handler/message_wait", 5000).toInt();
		int idCacheTtl = settings.value("handler/id_cache_ttl", 0).toInt();
		int clientMaxconn = settings.value("runner/client_maxconn", 50000).toInt();
		int connectionSubscriptionMax = settings.value("handler/connection_subscription_max", 20).toInt();
		int subscriptionLinger = settings.value("handler/subscription_linger", 60).toInt();
		int statsConnectionSend = settings.value("global/stats_connection_send", true).toBool();
		int statsConnectionTtl = settings.value("global/stats_connection_ttl", 120).toInt();
		int statsSubscriptionTtl = settings.value("handler/stats_subscription_ttl", 60).toInt();
		int statsReportInterval = settings.value("handler/stats_report_interval", 10).toInt();
		QString statsFormat = settings.value("handler/stats_format").toString();
		QString prometheusPort = settings.value("handler/prometheus_port").toString();
		QString prometheusPrefix = settings.value("handler/prometheus_prefix").toString();

		if(m2a_in_stream_specs.isEmpty() || m2a_out_specs.isEmpty())
		{
			log_error("must set m2a_in_stream_specs and m2a_out_specs");
			q->quit(0);
			return;
		}

		if(proxy_inspect_spec.isEmpty() || proxy_accept_spec.isEmpty() || proxy_retry_out_spec.isEmpty())
		{
			log_error("must set proxy_inspect_spec, proxy_accept_spec, and proxy_retry_out_spec");
			q->quit(0);
			return;
		}

		HandlerEngine::Configuration config;
		config.appVersion = Config::get().version;
		config.instanceId = "pushpin-handler_" + QByteArray::number(QCoreApplication::applicationPid());
		if(!services.contains("mongrel2") && (!condure_in_stream_specs.isEmpty() || !condure_out_specs.isEmpty()))
		{
			config.serverInStreamSpecs = condure_in_stream_specs;
			config.serverOutSpecs = condure_out_specs;
		}
		else
		{
			config.serverInStreamSpecs = m2a_in_stream_specs;
			config.serverOutSpecs = m2a_out_specs;
		}
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
		config.pushInSubSpecs = push_in_sub_specs;
		config.pushInSubConnect = push_in_sub_connect;
		config.pushInHttpAddr = QHostAddress(push_in_http_addr);
		config.pushInHttpPort = push_in_http_port;
		config.pushInHttpMaxHeadersSize = push_in_http_max_headers_size;
		config.pushInHttpMaxBodySize = push_in_http_max_body_size;
		config.ipcFileMode = ipcFileMode;
		config.shareAll = shareAll;
		config.messageRate = messageRate;
		config.messageHwm = messageHwm;
		config.messageBlockSize = messageBlockSize;
		config.messageWait = messageWait;
		config.idCacheTtl = idCacheTtl;
		config.connectionsMax = clientMaxconn;
		config.connectionSubscriptionMax = connectionSubscriptionMax;
		config.subscriptionLinger = subscriptionLinger;
		config.statsConnectionSend = statsConnectionSend;
		config.statsConnectionTtl = statsConnectionTtl;
		config.statsSubscriptionTtl = statsSubscriptionTtl;
		config.statsReportInterval = statsReportInterval;
		config.statsFormat = statsFormat;
		config.prometheusPort = prometheusPort;
		config.prometheusPrefix = prometheusPrefix;

		engine = new HandlerEngine(this);
		if(!engine->start(config))
		{
			q->quit(0);
			return;
		}

		log_info("started");
	}

private:
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
		q->quit(0);
	}
};

HandlerApp::HandlerApp(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

HandlerApp::~HandlerApp()
{
	delete d;
}

void HandlerApp::start()
{
	d->start();
}

#include "handlerapp.moc"
