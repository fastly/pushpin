/*
 * Copyright (C) 2015-2022 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
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
#include <QDir>
#include "timer.h"
#include "defercall.h"
#include "eventloop.h"
#include "processquit.h"
#include "log.h"
#include "simplehttpserver.h"
#include "httpsession.h"
#include "wssession.h"
#include "httpsessionupdatemanager.h"
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

static QStringList expandSpecs(const QStringList &l, int peerCount)
{
	if(l.count() == 1 && l[0].startsWith("ipc:") && peerCount > 1)
	{
		QString base = l[0];

		QStringList out;
		for(int i = 0; i < peerCount; ++i)
			out += base + QString("-%1").arg(i);

		return out;
	}

	return l;
}

static QString firstSpec(const QString &s, int peerCount)
{
	if(s.startsWith("ipc:") && peerCount > 1)
		return s + "-0";

	return s;
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

class HandlerApp::Private
{
public:
	static int run()
	{
		QCoreApplication::setApplicationName("pushpin-handler");
		QCoreApplication::setApplicationVersion(Config::get().version);

		QCommandLineParser parser;
		parser.setApplicationDescription("Pushpin handler component.");

		ArgsData args;
		QString errorMessage;
		switch(parseCommandLine(&parser, &args, &errorMessage))
		{
			case CommandLineOk:
				break;
			case CommandLineError:
				fprintf(stderr, "%s\n\n%s", qPrintable(errorMessage), qPrintable(parser.helpText()));
				return 1;
			case CommandLineVersionRequested:
				printf("%s %s\n", qPrintable(QCoreApplication::applicationName()),
					qPrintable(QCoreApplication::applicationVersion()));
				return 0;
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
				return 1;
			}
		}

		log_debug("starting...");

		QString configFile = args.configFile;
		if(configFile.isEmpty())
			configFile = QDir(Config::get().configDir).filePath("pushpin.conf");

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				log_error("failed to open %s, and --config not passed", qPrintable(configFile));
				return 1;
			}
		}

		Settings settings(configFile);

		if(!args.ipcPrefix.isEmpty())
			settings.setIpcPrefix(args.ipcPrefix);

		if(args.portOffset != -1)
			settings.setPortOffset(args.portOffset);

		QStringList services = settings.value("runner/services").toStringList();

		QStringList connmgr_in_stream_specs = settings.value("proxy/connmgr_in_stream_specs").toStringList();
		trimlist(&connmgr_in_stream_specs);
		QStringList connmgr_out_specs = settings.value("proxy/connmgr_out_specs").toStringList();
		trimlist(&connmgr_out_specs);
		QStringList condure_in_stream_specs = settings.value("proxy/condure_in_stream_specs").toStringList();
		trimlist(&condure_in_stream_specs);
		connmgr_in_stream_specs += condure_in_stream_specs;
		QStringList condure_out_specs = settings.value("proxy/condure_out_specs").toStringList();
		trimlist(&condure_out_specs);
		connmgr_out_specs += condure_out_specs;
		int proxyWorkerCount = settings.value("proxy/workers", 1).toInt();
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
		QStringList proxy_inspect_specs = settings.value("handler/proxy_inspect_specs").toStringList();
		trimlist(&proxy_inspect_specs);
		QString proxy_inspect_spec = settings.value("handler/proxy_inspect_spec").toString();
		if(!proxy_inspect_spec.isEmpty())
			proxy_inspect_specs += proxy_inspect_spec;
		QStringList proxy_accept_specs = settings.value("handler/proxy_accept_specs").toStringList();
		trimlist(&proxy_accept_specs);
		QString proxy_accept_spec = settings.value("handler/proxy_accept_spec").toString();
		if(!proxy_accept_spec.isEmpty())
			proxy_accept_specs += proxy_accept_spec;
		QStringList proxy_retry_out_specs = settings.value("handler/proxy_retry_out_specs").toStringList();
		trimlist(&proxy_retry_out_specs);
		QString proxy_retry_out_spec = settings.value("handler/proxy_retry_out_spec").toString();
		if(!proxy_retry_out_spec.isEmpty())
			proxy_retry_out_specs += proxy_retry_out_spec;
		QStringList ws_control_init_specs = settings.value("handler/proxy_ws_control_init_specs").toStringList();
		trimlist(&ws_control_init_specs);
		QStringList ws_control_stream_specs = settings.value("handler/proxy_ws_control_stream_specs").toStringList();
		trimlist(&ws_control_stream_specs);
		QString stats_spec = settings.value("handler/stats_spec").toString();
		QString command_spec = settings.value("handler/command_spec").toString();
		QString state_spec = settings.value("handler/state_spec").toString();
		QStringList proxy_stats_specs = settings.value("handler/proxy_stats_specs").toStringList();
		trimlist(&proxy_stats_specs);
		QString proxy_stats_spec = settings.value("handler/proxy_stats_spec").toString();
		if(!proxy_stats_spec.isEmpty())
			proxy_stats_specs += proxy_stats_spec;
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
		int push_in_http_max_headers_size = settings.value("handler/push_in_http_max_headers_size", DEFAULT_HTTP_MAX_HEADERS_SIZE).toInt();
		int push_in_http_max_body_size = settings.value("handler/push_in_http_max_body_size", DEFAULT_HTTP_MAX_BODY_SIZE).toInt();
		bool ok;
		int ipcFileMode = settings.value("handler/ipc_file_mode", -1).toString().toInt(&ok, 8);
		bool shareAll = settings.value("handler/share_all").toBool();
		int messageRate = settings.value("handler/message_rate", -1).toInt();
		int messageHwm = settings.value("handler/message_hwm", -1).toInt();
		int messageBlockSize = settings.value("handler/message_block_size", -1).toInt();
		int messageWait = settings.value("handler/message_wait", 5000).toInt();
		int idCacheTtl = settings.value("handler/id_cache_ttl", 0).toInt();
		bool updateOnFirstSubscription = settings.value("handler/update_on_first_subscription", true).toBool();
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
			return 1;
		}

		if(proxy_inspect_specs.isEmpty() || proxy_accept_specs.isEmpty() || proxy_retry_out_specs.isEmpty())
		{
			log_error("must set proxy_inspect_specs, proxy_accept_specs, and proxy_retry_out_specs");
			return 1;
		}

		HandlerEngine::Configuration config;
		config.appVersion = Config::get().version;
		config.instanceId = "handler_" + QByteArray::number(QCoreApplication::applicationPid());
		if(!services.contains("mongrel2") && (!connmgr_in_stream_specs.isEmpty() || !connmgr_out_specs.isEmpty()))
		{
			config.serverInStreamSpecs = connmgr_in_stream_specs;
			config.serverOutSpecs = connmgr_out_specs;
		}
		else
		{
			config.serverInStreamSpecs = m2a_in_stream_specs;
			config.serverOutSpecs = m2a_out_specs;
		}
		config.clientOutSpecs = expandSpecs(intreq_out_specs, proxyWorkerCount);
		config.clientOutStreamSpecs = expandSpecs(intreq_out_stream_specs, proxyWorkerCount);
		config.clientInSpecs = expandSpecs(intreq_in_specs, proxyWorkerCount);
		config.inspectSpecs = expandSpecs(proxy_inspect_specs, proxyWorkerCount);
		config.acceptSpecs = expandSpecs(proxy_accept_specs, proxyWorkerCount);
		config.retryOutSpecs = expandSpecs(proxy_retry_out_specs, proxyWorkerCount);
		config.wsControlInitSpecs = expandSpecs(ws_control_init_specs, proxyWorkerCount);
		config.wsControlStreamSpecs = expandSpecs(ws_control_stream_specs, proxyWorkerCount);
		config.statsSpec = stats_spec;
		config.commandSpec = command_spec;
		config.stateSpec = state_spec;
		config.proxyStatsSpecs = expandSpecs(proxy_stats_specs, proxyWorkerCount);
		config.proxyCommandSpec = firstSpec(proxy_command_spec, proxyWorkerCount);
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
		config.updateOnFirstSubscription = updateOnFirstSubscription;
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

		return runLoop(config, true);
	}

private:
	static int runLoop(const HandlerEngine::Configuration &config, bool newEventLoop)
	{
		// includes worst-case subscriptions and update registrations
		int timersPerSession = qMax(TIMERS_PER_HTTPSESSION, TIMERS_PER_WSSESSION) +
			(config.connectionSubscriptionMax * TIMERS_PER_SUBSCRIPTION) +
			TIMERS_PER_UNIQUE_UPDATE_REGISTRATION;

		// enough timers for sessions, plus an extra 100 for misc
		int timersMax = (config.connectionsMax * timersPerSession) + 100;

		std::unique_ptr<EventLoop> loop;

		if(newEventLoop)
		{
			log_debug("using new event loop");

			// enough for control requests and prometheus requests, plus an
			// extra 100 for misc. client sessions don't use socket notifiers
			int socketNotifiersMax = (SOCKETNOTIFIERS_PER_SIMPLEHTTPREQUEST * (CONTROL_CONNECTIONS_MAX + PROMETHEUS_CONNECTIONS_MAX)) + 100;

			int registrationsMax = timersMax + socketNotifiersMax;
			loop = std::make_unique<EventLoop>(registrationsMax);
		}
		else
		{
			// for qt event loop, timer subsystem must be explicitly initialized
			Timer::init(timersMax);
		}

		std::unique_ptr<HandlerEngine> engine;

		DeferCall deferCall;
		deferCall.defer([&] {
			engine = std::make_unique<HandlerEngine>();

			ProcessQuit::instance()->quit.connect([&] {
				log_info("stopping...");
		
				// remove the handler, so if we get another signal then we crash out
				ProcessQuit::cleanup();

				engine.reset();

				log_debug("stopped");

				if(newEventLoop)
					loop->exit(0);
				else
					QCoreApplication::exit(0);
			});

			ProcessQuit::instance()->hup.connect([&] {
				log_info("reloading");
				log_rotate();
				engine->reload();
			});

			if(!engine->start(config))
			{
				engine.reset();

				if(newEventLoop)
					loop->exit(1);
				else
					QCoreApplication::exit(1);

				return;
			}

			log_info("started");
		});

		int ret;
		if(newEventLoop)
			ret = loop->exec();
		else
			ret = QCoreApplication::exec();

		if(!newEventLoop)
		{
			// ensure deferred deletes are processed
			QCoreApplication::instance()->sendPostedEvents();
		}

		// deinit here, after all event loop activity has completed

		DeferCall::cleanup();

		if(!newEventLoop)
			Timer::deinit();

		return ret;
	}
};

HandlerApp::HandlerApp() = default;

HandlerApp::~HandlerApp() = default;

int HandlerApp::run()
{
	return Private::run();
}
