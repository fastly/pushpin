/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 * Copyright (C) 2023-2024 Fastly, Inc.
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
#include "xffrule.h"
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

static XffRule parse_xffRule(const QStringList &in)
{
	XffRule out;
	foreach(const QString &s, in)
	{
		if(s.startsWith("truncate:"))
		{
			bool ok;
			int x = s.mid(9).toInt(&ok);
			if(!ok)
				return out;

			out.truncate = x;
		}
		else if(s == "append")
			out.append = true;
	}
	return out;
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
	QStringList routeLines;
	bool quietCheck;

	ArgsData() :
		logLevel(-1),
		quietCheck(false)
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
	const QCommandLineOption routeOption("route", "Add route (overrides routes file).", "line");
	parser->addOption(routeOption);
	const QCommandLineOption quietCheckOption("quiet-check", "Log update checks in Zurl as debug level.");
	parser->addOption(quietCheckOption);
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

	if(parser->isSet(routeOption))
	{
		foreach(const QString &r, parser->values(routeOption))
			args->routeLines += r;
	}

	if(parser->isSet(quietCheckOption))
		args->quietCheck = true;

	return CommandLineOk;
}

class App::Private : public QObject
{
	Q_OBJECT

public:
	App *q;
	ArgsData args;
	Engine *engine;
	Connection quitConnection;
	Connection hupConnection;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		engine(0)
	{
		quitConnection = ProcessQuit::instance()->quit.connect(boost::bind(&Private::doQuit, this));
		hupConnection = ProcessQuit::instance()->hup.connect(boost::bind(&App::Private::reload, this));
	}

	void start()
	{
		QCoreApplication::setApplicationName("pushpin-proxy");
		QCoreApplication::setApplicationVersion(Config::get().version);

		QCommandLineParser parser;
		parser.setApplicationDescription("Pushpin proxy component.");

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

		QDir configDir = QFileInfo(configFile).absoluteDir();

		Settings settings(configFile);

		if(!args.ipcPrefix.isEmpty())
			settings.setIpcPrefix(args.ipcPrefix);

		QStringList services = settings.value("runner/services").toStringList();

		QStringList condure_in_specs = settings.value("proxy/condure_in_specs").toStringList();
		trimlist(&condure_in_specs);
		QStringList condure_in_stream_specs = settings.value("proxy/condure_in_stream_specs").toStringList();
		trimlist(&condure_in_stream_specs);
		QStringList condure_out_specs = settings.value("proxy/condure_out_specs").toStringList();
		trimlist(&condure_out_specs);
		QStringList m2a_in_specs = settings.value("proxy/m2a_in_specs").toStringList();
		trimlist(&m2a_in_specs);
		QStringList m2a_in_stream_specs = settings.value("proxy/m2a_in_stream_specs").toStringList();
		trimlist(&m2a_in_stream_specs);
		QStringList m2a_out_specs = settings.value("proxy/m2a_out_specs").toStringList();
		trimlist(&m2a_out_specs);
		QStringList condure_client_out_specs = settings.value("proxy/condure_client_out_specs").toStringList();
		trimlist(&condure_client_out_specs);
		QStringList condure_client_out_stream_specs = settings.value("proxy/condure_client_out_stream_specs").toStringList();
		trimlist(&condure_client_out_stream_specs);
		QStringList condure_client_in_specs = settings.value("proxy/condure_client_in_specs").toStringList();
		trimlist(&condure_client_in_specs);
		QStringList zurl_out_specs = settings.value("proxy/zurl_out_specs").toStringList();
		trimlist(&zurl_out_specs);
		QStringList zurl_out_stream_specs = settings.value("proxy/zurl_out_stream_specs").toStringList();
		trimlist(&zurl_out_stream_specs);
		QStringList zurl_in_specs = settings.value("proxy/zurl_in_specs").toStringList();
		trimlist(&zurl_in_specs);
		QString handler_inspect_spec = settings.value("proxy/handler_inspect_spec").toString();
		QString handler_accept_spec = settings.value("proxy/handler_accept_spec").toString();
		QString handler_retry_in_spec = settings.value("proxy/handler_retry_in_spec").toString();
		QStringList handler_ws_control_init_specs = settings.value("proxy/handler_ws_control_init_specs").toStringList();
		trimlist(&handler_ws_control_init_specs);
		QStringList handler_ws_control_stream_specs = settings.value("proxy/handler_ws_control_stream_specs").toStringList();
		trimlist(&handler_ws_control_stream_specs);
		QString stats_spec = settings.value("proxy/stats_spec").toString();
		QString command_spec = settings.value("proxy/command_spec").toString();
		QStringList intreq_in_specs = settings.value("proxy/intreq_in_specs").toStringList();
		trimlist(&intreq_in_specs);
		QStringList intreq_in_stream_specs = settings.value("proxy/intreq_in_stream_specs").toStringList();
		trimlist(&intreq_in_stream_specs);
		QStringList intreq_out_specs = settings.value("proxy/intreq_out_specs").toStringList();
		trimlist(&intreq_out_specs);
		bool ok;
		int ipcFileMode = settings.value("proxy/ipc_file_mode", -1).toString().toInt(&ok, 8);
		int sessionsMax = settings.value("proxy/max_open_requests", -1).toInt();
		QString routesFile = settings.value("proxy/routesfile").toString();
		bool debug = settings.value("proxy/debug").toBool();
		bool autoCrossOrigin = settings.value("proxy/auto_cross_origin").toBool();
		bool acceptXForwardedProtocol = settings.value("proxy/accept_x_forwarded_protocol").toBool();
		QString setXForwardedProtocol = settings.value("proxy/set_x_forwarded_protocol").toString();
		bool setXfProto = (setXForwardedProtocol == "true" || setXForwardedProtocol == "proto-only");
		bool setXfProtocol = (setXForwardedProtocol == "true");
		XffRule xffRule = parse_xffRule(settings.value("proxy/x_forwarded_for").toStringList());
		XffRule xffTrustedRule = parse_xffRule(settings.value("proxy/x_forwarded_for_trusted").toStringList());
		QStringList origHeadersNeedMarkStr = settings.value("proxy/orig_headers_need_mark").toStringList();
		trimlist(&origHeadersNeedMarkStr);
		bool acceptPushpinRoute = settings.value("proxy/accept_pushpin_route").toBool();
		QByteArray cdnLoop = settings.value("proxy/cdn_loop").toString().toUtf8();
		bool logFrom = settings.value("proxy/log_from").toBool();
		bool logUserAgent = settings.value("proxy/log_user_agent").toBool();
		QByteArray sigIss = settings.value("proxy/sig_iss", "pushpin").toString().toUtf8();
		Jwt::EncodingKey sigKey = Jwt::EncodingKey::fromConfigString(settings.value("proxy/sig_key").toString(), configDir);
		Jwt::DecodingKey upstreamKey = Jwt::DecodingKey::fromConfigString(settings.value("proxy/upstream_key").toString(), configDir);
		QString sockJsUrl = settings.value("proxy/sockjs_url").toString();
		QString updatesCheck = settings.value("proxy/updates_check").toString();
		QString organizationName = settings.value("proxy/organization_name").toString();
		int clientMaxconn = settings.value("runner/client_maxconn", 50000).toInt();
		bool statsConnectionSend = settings.value("global/stats_connection_send", true).toBool();
		int statsConnectionTtl = settings.value("global/stats_connection_ttl", 120).toInt();
		int statsConnectionsMaxTtl = settings.value("proxy/stats_connections_max_ttl", 60).toInt();
		int statsReportInterval = settings.value("proxy/stats_report_interval", 10).toInt();
		QString prometheusPort = settings.value("proxy/prometheus_port").toString();
		QString prometheusPrefix = settings.value("proxy/prometheus_prefix").toString();

		QList<QByteArray> origHeadersNeedMark;
		foreach(const QString &s, origHeadersNeedMarkStr)
			origHeadersNeedMark += s.toUtf8();

		// if routesfile is a relative path, then use it relative to the config file location
		QFileInfo fi(routesFile);
		if(fi.isRelative())
			routesFile = QFileInfo(configDir, routesFile).filePath();

		if(!(!condure_in_specs.isEmpty() && !condure_in_stream_specs.isEmpty() && !condure_out_specs.isEmpty()) && !(!m2a_in_specs.isEmpty() && !m2a_in_stream_specs.isEmpty() && !m2a_out_specs.isEmpty()))
		{
			log_error("must set condure_in_specs, condure_in_stream_specs, and condure_out_specs, or m2a_in_specs, m2a_in_stream_specs, and m2a_out_specs");
			q->quit(0);
			return;
		}

		if(!(!condure_client_out_specs.isEmpty() && !condure_client_out_stream_specs.isEmpty() && !condure_client_in_specs.isEmpty()) && !(!zurl_out_specs.isEmpty() && !zurl_out_stream_specs.isEmpty() && !zurl_in_specs.isEmpty()))
		{
			log_error("must set condure_client_out_specs, condure_client_out_stream_specs, and condure_client_in_specs, or zurl_out_specs, zurl_out_stream_specs, and zurl_in_specs");
			q->quit(0);
			return;
		}

		if(updatesCheck == "true")
			updatesCheck = "check";

		// sessionsMax should not exceed clientMaxconn
		if(sessionsMax >= 0)
			sessionsMax = qMin(sessionsMax, clientMaxconn);
		else
			sessionsMax = clientMaxconn;

		Engine::Configuration config;
		config.appVersion = Config::get().version;
		config.clientId = "pushpin-proxy_" + QByteArray::number(QCoreApplication::applicationPid());
		if(!services.contains("mongrel2") && (!condure_in_specs.isEmpty() || !condure_in_stream_specs.isEmpty() || !condure_out_specs.isEmpty()))
		{
			config.serverInSpecs = condure_in_specs;
			config.serverInStreamSpecs = condure_in_stream_specs;
			config.serverOutSpecs = condure_out_specs;
		}
		else
		{
			config.serverInSpecs = m2a_in_specs;
			config.serverInStreamSpecs = m2a_in_stream_specs;
			config.serverOutSpecs = m2a_out_specs;
		}
		if(!services.contains("zurl") && (!condure_client_out_specs.isEmpty() || !condure_client_out_stream_specs.isEmpty() || !condure_client_in_specs.isEmpty()))
		{
			config.clientOutSpecs = condure_client_out_specs;
			config.clientOutStreamSpecs = condure_client_out_stream_specs;
			config.clientInSpecs = condure_client_in_specs;
		}
		else
		{
			config.clientOutSpecs = zurl_out_specs;
			config.clientOutStreamSpecs = zurl_out_stream_specs;
			config.clientInSpecs = zurl_in_specs;
		}
		config.inspectSpec = handler_inspect_spec;
		config.acceptSpec = handler_accept_spec;
		config.retryInSpec = handler_retry_in_spec;
		config.wsControlInitSpecs = handler_ws_control_init_specs;
		config.wsControlStreamSpecs = handler_ws_control_stream_specs;
		config.statsSpec = stats_spec;
		config.commandSpec = command_spec;
		config.intServerInSpecs = intreq_in_specs;
		config.intServerInStreamSpecs = intreq_in_stream_specs;
		config.intServerOutSpecs = intreq_out_specs;
		config.ipcFileMode = ipcFileMode;
		config.sessionsMax = sessionsMax;
		if(!args.routeLines.isEmpty())
			config.routeLines = args.routeLines;
		else
			config.routesFile = routesFile;
		config.debug = debug;
		config.autoCrossOrigin = autoCrossOrigin;
		config.acceptXForwardedProto = acceptXForwardedProtocol;
		config.setXForwardedProto = setXfProto;
		config.setXForwardedProtocol = setXfProtocol;
		config.xffUntrustedRule = xffRule;
		config.xffTrustedRule = xffTrustedRule;
		config.origHeadersNeedMark = origHeadersNeedMark;
		config.acceptPushpinRoute = acceptPushpinRoute;
		config.cdnLoop = cdnLoop;
		config.logFrom = logFrom;
		config.logUserAgent = logUserAgent;
		config.sigIss = sigIss;
		config.sigKey = sigKey;
		config.upstreamKey = upstreamKey;
		config.sockJsUrl = sockJsUrl;
		config.updatesCheck = updatesCheck;
		config.organizationName = organizationName;
		config.quietCheck = args.quietCheck;
		config.statsConnectionSend = statsConnectionSend;
		config.statsConnectionTtl = statsConnectionTtl;
		config.statsConnectionsMaxTtl = statsConnectionsMaxTtl;
		config.statsReportInterval = statsReportInterval;
		config.prometheusPort = prometheusPort;
		config.prometheusPrefix = prometheusPrefix;

		engine = new Engine(this);
		if(!engine->start(config))
		{
			q->quit(0);
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
		q->quit(0);
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
