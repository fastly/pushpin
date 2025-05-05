/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
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
#include "timer.h"
#include "defercall.h"
#include "log.h"
#include "settings.h"
#include "xffrule.h"
#include "domainmap.h"
#include "engine.h"
#include "config.h"
#include "cacheutil.h"

extern bool gCacheThreadAllowFlag;
extern redisContext *gRedisContext;

using Connection = boost::signals2::scoped_connection;

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

static QString suffixSpec(const QString &s, int i)
{
	if(s.startsWith("ipc:"))
		return s + QString("-%1").arg(i);

	return s;
}

static QStringList suffixSpecs(const QStringList &l, int i)
{
	if(l.count() == 1 && l[0].startsWith("ipc:"))
		return QStringList() << (l[0] + QString("-%1").arg(i));

	return l;
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

class EngineWorker : public QObject
{
	Q_OBJECT

public:
	EngineWorker(const Engine::Configuration &config, DomainMap *domainMap) :
		QObject(),
		config_(config),
		engine_(new Engine(domainMap, this))
	{
	}

	Signal started;
	Signal stopped;
	Signal error;

public slots:
	void start()
	{
		if(!engine_->start(config_))
		{
			delete engine_;
			engine_ = 0;

			error();
			return;
		}

		started();
	}

	void stop()
	{
		delete engine_;
		engine_ = 0;

		stopped();
	}

	void routesChanged()
	{
		if(engine_)
			engine_->routesChanged();
	}

private:
	Engine::Configuration config_;
	Engine *engine_;
};

class EngineThread : public QThread
{
	Q_OBJECT

public:
	QMutex m;
	QWaitCondition w;
	Engine::Configuration config;
	DomainMap *domainMap;
	EngineWorker *worker;

	EngineThread(const Engine::Configuration &_config, DomainMap *_domainMap) :
		config(_config),
		domainMap(_domainMap),
		worker(0)
	{
	}

	~EngineThread()
	{
		stop();
		wait();
	}

	bool start()
	{
		setObjectName("proxy-worker-" + QString::number(config.id));

		QMutexLocker locker(&m);
		QThread::start();
		w.wait(&m);
		return (bool)worker;
	}

	void stop()
	{
		QMutexLocker locker(&m);

		if(worker)
			QMetaObject::invokeMethod(worker, "stop", Qt::QueuedConnection);
	}

	void routesChanged()
	{
		QMutexLocker locker(&m);

		if(worker)
			QMetaObject::invokeMethod(worker, "routesChanged", Qt::QueuedConnection);
	}

	virtual void run()
	{
		// will unlock during exec
		m.lock();

		worker = new EngineWorker(config, domainMap);
		Connection startedConnection = worker->started.connect(boost::bind(&EngineThread::worker_started, this));
		Connection stoppedConnection = worker->stopped.connect(boost::bind(&EngineThread::worker_stopped, this));
		Connection errorConnection = worker->error.connect(boost::bind(&EngineThread::worker_error, this));
		QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);
		exec();

		// ensure deferred deletes are processed
		QCoreApplication::instance()->sendPostedEvents();

		// deinit here, after all event loop activity has completed
		Timer::deinit();
		DeferCall::cleanup();
	}

private:
	void worker_started()
	{
		log_debug("worker %d: started", config.id);

		// unblock start()
		w.wakeOne();
		m.unlock();
	}

	void worker_stopped()
	{
		delete worker;
		worker = 0;

		log_debug("worker %d: stopped", config.id);

		quit();
	}

	void worker_error()
	{
		delete worker;
		worker = 0;

		quit();

		// unblock start()
		w.wakeOne();
		m.unlock();
	}
};

class App::Private : public QObject
{
	Q_OBJECT

public:
	App *q;
	ArgsData args;
	DomainMap *domainMap;
	std::list<EngineThread*> threads;
	Connection quitConnection;
	Connection hupConnection;
	Connection changedConnection;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		domainMap(0)
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
				q->quit(0);
				return;
			}
		}

		QDir configDir = QFileInfo(configFile).absoluteDir();

		Settings settings(configFile);

		if(!args.ipcPrefix.isEmpty())
			settings.setIpcPrefix(args.ipcPrefix);

		QStringList services = settings.value("runner/services").toStringList();

		int workerCount = settings.value("proxy/workers", 1).toInt();
		QStringList connmgr_in_specs = settings.value("proxy/connmgr_in_specs").toStringList();
		trimlist(&connmgr_in_specs);
		QStringList connmgr_in_stream_specs = settings.value("proxy/connmgr_in_stream_specs").toStringList();
		trimlist(&connmgr_in_stream_specs);
		QStringList connmgr_out_specs = settings.value("proxy/connmgr_out_specs").toStringList();
		trimlist(&connmgr_out_specs);
		QStringList condure_in_specs = settings.value("proxy/condure_in_specs").toStringList();
		trimlist(&condure_in_specs);
		connmgr_in_specs += condure_in_specs;
		QStringList condure_in_stream_specs = settings.value("proxy/condure_in_stream_specs").toStringList();
		trimlist(&condure_in_stream_specs);
		connmgr_in_stream_specs += condure_in_stream_specs;
		QStringList condure_out_specs = settings.value("proxy/condure_out_specs").toStringList();
		trimlist(&condure_out_specs);
		connmgr_out_specs += condure_out_specs;
		QStringList m2a_in_specs = settings.value("proxy/m2a_in_specs").toStringList();
		trimlist(&m2a_in_specs);
		QStringList m2a_in_stream_specs = settings.value("proxy/m2a_in_stream_specs").toStringList();
		trimlist(&m2a_in_stream_specs);
		QStringList m2a_out_specs = settings.value("proxy/m2a_out_specs").toStringList();
		trimlist(&m2a_out_specs);
		QStringList connmgr_client_out_specs = settings.value("proxy/connmgr_client_out_specs").toStringList();
		trimlist(&connmgr_client_out_specs);
		QStringList connmgr_client_out_stream_specs = settings.value("proxy/connmgr_client_out_stream_specs").toStringList();
		trimlist(&connmgr_client_out_stream_specs);
		QStringList connmgr_client_in_specs = settings.value("proxy/connmgr_client_in_specs").toStringList();
		trimlist(&connmgr_client_in_specs);
		QStringList condure_client_out_specs = settings.value("proxy/condure_client_out_specs").toStringList();
		trimlist(&condure_client_out_specs);
		connmgr_client_out_specs += condure_client_out_specs;
		QStringList condure_client_out_stream_specs = settings.value("proxy/condure_client_out_stream_specs").toStringList();
		trimlist(&condure_client_out_stream_specs);
		connmgr_client_out_stream_specs += condure_client_out_stream_specs;
		QStringList condure_client_in_specs = settings.value("proxy/condure_client_in_specs").toStringList();
		trimlist(&condure_client_in_specs);
		connmgr_client_in_specs += condure_client_in_specs;
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

		if(!(!connmgr_in_specs.isEmpty() && !connmgr_in_stream_specs.isEmpty() && !connmgr_out_specs.isEmpty()) && !(!m2a_in_specs.isEmpty() && !m2a_in_stream_specs.isEmpty() && !m2a_out_specs.isEmpty()))
		{
			log_error("must set connmgr_in_specs, connmgr_in_stream_specs, and connmgr_out_specs, or m2a_in_specs, m2a_in_stream_specs, and m2a_out_specs");
			q->quit(0);
			return;
		}

		if(!(!connmgr_client_out_specs.isEmpty() && !connmgr_client_out_stream_specs.isEmpty() && !connmgr_client_in_specs.isEmpty()) && !(!zurl_out_specs.isEmpty() && !zurl_out_stream_specs.isEmpty() && !zurl_in_specs.isEmpty()))
		{
			log_error("must set connmgr_client_out_specs, connmgr_client_out_stream_specs, and connmgr_client_in_specs, or zurl_out_specs, zurl_out_stream_specs, and zurl_in_specs");
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

		if(!args.routeLines.isEmpty())
		{
			domainMap = new DomainMap(this);
			foreach(const QString &line, args.routeLines)
				domainMap->addRouteLine(line);
		}
		else
			domainMap = new DomainMap(routesFile, this);

		changedConnection = domainMap->changed.connect(boost::bind(&Private::domainMap_changed, this));

		Engine::Configuration config;
		config.appVersion = Config::get().version;
		config.clientId = "proxy_" + QByteArray::number(QCoreApplication::applicationPid());
		if(!services.contains("mongrel2") && (!connmgr_in_specs.isEmpty() || !connmgr_in_stream_specs.isEmpty() || !connmgr_out_specs.isEmpty()))
		{
			config.serverInSpecs = connmgr_in_specs;
			config.serverInStreamSpecs = connmgr_in_stream_specs;
			config.serverOutSpecs = connmgr_out_specs;
		}
		else
		{
			config.serverInSpecs = m2a_in_specs;
			config.serverInStreamSpecs = m2a_in_stream_specs;
			config.serverOutSpecs = m2a_out_specs;
		}
		if(!services.contains("zurl") && (!connmgr_client_out_specs.isEmpty() || !connmgr_client_out_stream_specs.isEmpty() || !connmgr_client_in_specs.isEmpty()))
		{
			config.clientOutSpecs = connmgr_client_out_specs;
			config.clientOutStreamSpecs = connmgr_client_out_stream_specs;
			config.clientInSpecs = connmgr_client_in_specs;
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
		config.sessionsMax = sessionsMax / workerCount;
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

		// Cache config
		bool cacheEnable = settings.value("cache/cache_enable").toBool();
		QStringList httpBackendUrlList = settings.value("cache/http_backend_urls").toStringList();
		QStringList wsBackendUrlList = settings.value("cache/ws_backend_urls").toStringList();
		QStringList cacheMethodList = settings.value("cache/ws_cache_methods").toStringList();
		QStringList subscribeMethodList = settings.value("cache/ws_subscribe_methods").toStringList();
		QStringList neverTimeoutMethodList = settings.value("cache/ws_never_timeout_methods").toStringList();
		QStringList refreshUneraseMethodList = settings.value("cache/ws_refresh_unerase_methods").toStringList();
		QStringList refreshExcludeMethodList = settings.value("cache/ws_refresh_exclude_methods").toStringList();
		QStringList refreshPassthroughMethodList = settings.value("cache/ws_refresh_passthrough_methods").toStringList();
		QString cacheKeyConfig = settings.value("cache/ws_cache_key", "").toString().simplified().remove("'").remove("\"").toLower();
		QStringList cacheKeyParts = cacheKeyConfig.split(u'+', QString::SkipEmptyParts);
		QStringList cacheKeyItemList;
		for (int i = 0; i < cacheKeyParts.count(); i++)
		{
			QString keyPart = cacheKeyParts[i].trimmed();
			if (keyPart.startsWith("$request_json_value[") && keyPart.endsWith("]"))
			{
				QString jsonValue = keyPart.mid(20, keyPart.length()-20-1).trimmed();
				jsonValue += ".JSON_VALUE";
				cacheKeyItemList.append(jsonValue);
			}
			else if (keyPart.startsWith("$request_json_pair[") && keyPart.endsWith("]"))
			{
				QString jsonValue = keyPart.mid(19, keyPart.length()-19-1).trimmed();
				jsonValue += ".JSON_PAIR";
				cacheKeyItemList.append(jsonValue);
			}
			else if (keyPart.startsWith("$user_defined[") && keyPart.endsWith("]"))
			{
				QString jsonValue = keyPart.mid(14, keyPart.length()-14-1).trimmed();
				QString userDefinedKeyConfig = settings.value("cache/"+jsonValue, "").toString().simplified().remove("'").remove("\"").toLower();
				QStringList userDefinedKeyParts = userDefinedKeyConfig.split(u'+', QString::SkipEmptyParts);
				for (int j = 0; j < userDefinedKeyParts.count(); j++)
				{
					QString userDefinedKeyPart = userDefinedKeyParts[j].trimmed();
					if (userDefinedKeyPart.startsWith("$request_json_value[") && userDefinedKeyPart.endsWith("]"))
					{
						jsonValue = userDefinedKeyPart.mid(20, userDefinedKeyPart.length()-20-1).trimmed();
						jsonValue += ".JSON_VALUE";
						cacheKeyItemList.append(jsonValue);
					}
					else if (userDefinedKeyPart.startsWith("$request_json_pair[") && userDefinedKeyPart.endsWith("]"))
					{
						jsonValue = userDefinedKeyPart.mid(19, userDefinedKeyPart.length()-19-1).trimmed();
						jsonValue += ".JSON_PAIR";
						cacheKeyItemList.append(jsonValue);
					}
					else
					{
						userDefinedKeyPart += ".RAW_VALUE";
						cacheKeyItemList.append(userDefinedKeyPart);
					}
				}
			}
			else
			{
				keyPart += ".RAW_VALUE";
				cacheKeyItemList.append(keyPart);
			}
		}
		// message iden attribute and cache check attribute
		QString msgIdFieldName = settings.value("cache/message_id_attribute", "").toString().simplified().remove("'").remove("\"").toLower();
		QString msgMethodFieldName = settings.value("cache/message_method_attribute", "").toString().simplified().remove("'").remove("\"").toLower();
		QString msgParamsFieldName = settings.value("cache/message_params_attribute", "params").toString().simplified().remove("'").remove("\"").toLower();
		// redis
		bool redisEnable = settings.value("cache/redis_enable").toBool();
		QString redisHostAddr = settings.value("cache/redis_host_addr").toString();
		int redisPort = settings.value("cache/redis_port", 6379).toInt();
		// count method group
		QStringList countMethodGroups = settings.value("cache/ws_count_groups").toStringList();
		QMap<QString, QStringList> countMethodGroupMap;
		for (int i = 0; i < countMethodGroups.count(); i++)
		{
			QString groupKey = countMethodGroups[i];
			QStringList groupValue = settings.value("cache/" + groupKey).toStringList();
			countMethodGroupMap[groupKey] = groupValue;
		}

		config.cacheEnable = cacheEnable;
		config.httpBackendUrlList = httpBackendUrlList;
		config.wsBackendUrlList = wsBackendUrlList;
		config.cacheMethodList = cacheMethodList;
		config.subscribeMethodList = subscribeMethodList;
		config.neverTimeoutMethodList = neverTimeoutMethodList;
		config.refreshUneraseMethodList = refreshUneraseMethodList;
		config.refreshExcludeMethodList = refreshExcludeMethodList;
		config.refreshPassthroughMethodList = refreshPassthroughMethodList;
		config.cacheKeyItemList = cacheKeyItemList;
		config.msgIdFieldName = msgIdFieldName;
		config.msgMethodFieldName = msgMethodFieldName;
		config.msgParamsFieldName = msgParamsFieldName;
		config.redisEnable = redisEnable;
		config.redisHostAddr = redisHostAddr;
		config.redisPort = redisPort;
		config.countMethodGroupMap = countMethodGroupMap;

		for(int n = 0; n < workerCount; ++n)
		{
			Engine::Configuration wconfig = config;

			wconfig.id = n;

			if(workerCount > 1)
			{
				wconfig.clientId += '-' + QByteArray::number(n);

				wconfig.inspectSpec = suffixSpec(wconfig.inspectSpec, n);
				wconfig.acceptSpec = suffixSpec(wconfig.acceptSpec, n);
				wconfig.retryInSpec = suffixSpec(wconfig.retryInSpec, n);
				wconfig.wsControlInitSpecs = suffixSpecs(wconfig.wsControlInitSpecs, n);
				wconfig.wsControlStreamSpecs = suffixSpecs(wconfig.wsControlStreamSpecs, n);
				wconfig.statsSpec = suffixSpec(wconfig.statsSpec, n);
				wconfig.commandSpec = suffixSpec(wconfig.commandSpec, n);
				wconfig.intServerInSpecs = suffixSpecs(wconfig.intServerInSpecs, n);
				wconfig.intServerInStreamSpecs = suffixSpecs(wconfig.intServerInStreamSpecs, n);
				wconfig.intServerOutSpecs = suffixSpecs(wconfig.intServerOutSpecs, n);
			}

			EngineThread *t = new EngineThread(wconfig, domainMap);
			if(!t->start())
			{
				delete t;

				for(EngineThread *t : threads)
					delete t;

				threads.clear();

				q->quit(0);
				return;
			}

			threads.push_back(t);
		}

		log_info("started");
	}

private:
	void domainMap_changed()
	{
		for(EngineThread *t : threads)
			t->routesChanged();
	}

private slots:
	void reload()
	{
		log_info("reloading");
		log_rotate();

		domainMap->reload();
	}

	void doQuit()
	{
		log_info("stopping...");

		// remove the handler, so if we get another signal then we crash out
		ProcessQuit::cleanup();

		for(EngineThread *t : threads)
			t->stop();

		for(EngineThread *t : threads)
			delete t;

		threads.clear();

		// free redis
		if (gRedisContext != nullptr)
			redisFree(gRedisContext);

		gCacheThreadAllowFlag = false;

		log_debug("stopped");
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
