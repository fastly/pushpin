/*
 * Copyright (C) 2016-2025 Fanout, Inc.
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

#include "runnerapp.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QStringList>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QUrlQuery>
#include "rust/bindings.h"
#include "processquit.h"
#include "log.h"
#include "settings.h"
#include "listenport.h"
#include "connmgrservice.h"
#include "mongrel2service.h"
#include "m2adapterservice.h"
#include "zurlservice.h"
#include "pushpinproxyservice.h"
#include "pushpinhandlerservice.h"
#include "config.h"

struct ServiceConnections{
	Connection startedConnection;
	Connection stoppedConnection;
	Connection logConnection;
	Connection errConnection;
};

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

static bool ensureDir(const QString &path)
{
	QDir dir(path);
	if(dir.exists())
		return true;

	return QDir().mkpath(dir.absolutePath());
}

static QPair<QHostAddress, int> parsePort(const QString &s)
{
	// if the string doesn't contain a colon character, assume it's a port
	// number by itself
	if(!s.contains(':'))
		return QPair<QHostAddress, int>(QHostAddress(), s.toInt());

	// otherwise, assume it's an address:port combination

	// parse with QUrl in order to support bracketed IPv6 notation
	QUrl url{QUrl::fromUserInput(s)};

	return QPair<QHostAddress, int>(QHostAddress(url.host()), url.port());
}

QMap<QString, int> parseLogLevel(const QStringList &parts, QString *errorMessage)
{
	QMap<QString, int> levels;

	foreach(const QString &part, parts)
	{
		if(part.isEmpty())
		{
			*errorMessage = "log level component cannot be empty";
			return QMap<QString, int>();
		}

		int at = part.indexOf(':');
		if(at != -1)
		{
			if(at == 0)
			{
				*errorMessage = "log level component name cannot be empty";
				return QMap<QString, int>();
			}

			QString name = part.mid(0, at);

			bool ok;
			int x = part.mid(at + 1).toInt(&ok);
			if(!ok || x < 0)
			{
				*errorMessage = QString("log level for service %1 must be greater than or equal to 0").arg(name);
				return QMap<QString, int>();
			}

			levels[name] = x;
		}
		else
		{
			bool ok;
			int x = part.toInt(&ok);
			if(!ok || x < 0)
			{
				*errorMessage = "log level must be greater than or equal to 0";
				return QMap<QString, int>();
			}

			levels[""] = x;
		}
	}

	return levels;
}

enum CommandLineParseResult
{
	CommandLineOk,
	CommandLineError,
	CommandLineVersionRequested,
	CommandLineHelpRequested
};

class RunnerArgsData
{
public:
	QString configFile;
	QString logFile;
	QMap<QString, int> logLevels;
	bool mergeOutput;
	QPair<QHostAddress,int> port;
	int id;
	QStringList routeLines;

	RunnerArgsData() :
		mergeOutput(false),
		id(-1)
	{
	}
};

static CommandLineParseResult parseCommandLine(QCommandLineParser *parser, RunnerArgsData *args, QString *errorMessage)
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
	const QCommandLineOption mergeOutputOption(QStringList() << "m" << "merge-output", "Combine output of subprocesses.");
	parser->addOption(mergeOutputOption);
	const QCommandLineOption portOption("port", "Run a single HTTP server instance.", "[addr:]port");
	parser->addOption(portOption);
	const QCommandLineOption idOption("id", "Set instance ID (needed to run multiple instances).", "x");
	parser->addOption(idOption);
	const QCommandLineOption routeOption("route", "Add route (overrides routes file).", "line");
	parser->addOption(routeOption);
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
		QStringList parts = parser->value(logLevelOption).split(',');
		QMap<QString, int> levels = parseLogLevel(parts, errorMessage);
		if(levels.isEmpty())
		{
			return CommandLineError;
		}

		args->logLevels = levels;
	}

	if(parser->isSet(verboseOption))
	{
		args->logLevels.clear();
		args->logLevels[""] = 3;
	}

	if(parser->isSet(mergeOutputOption))
		args->mergeOutput = true;

	if(parser->isSet(portOption))
	{
		args->port = parsePort(parser->value(portOption));
		if(args->port.second < 1)
		{
			*errorMessage = "port must be greater than or equal to 1";
			return CommandLineError;
		}
	}

	if(parser->isSet(idOption))
	{
		bool ok;
		int x = parser->value(idOption).toInt(&ok);
		if(!ok || x < 0)
		{
			*errorMessage = "id must be greater than or equal to 0";
			return CommandLineError;
		}

		args->id = x;
	}

	if(parser->isSet(routeOption))
	{
		foreach(const QString &r, parser->values(routeOption))
			args->routeLines += r;
	}

	return CommandLineOk;
}

class RunnerApp::Private
{
public:
	RunnerApp *q;
	RunnerArgsData args;
	QList<Service*> services;
	bool stopping;
	bool errored;
	Connection quitConnection;
	Connection hupConnection;
	map<Service*, ServiceConnections> serviceConnectionMap;

	Private(RunnerApp *_q) :
		q(_q),
		stopping(false),
		errored(false)
	{
		quitConnection = ProcessQuit::instance()->quit.connect(boost::bind(&Private::processQuit, this));
		hupConnection = ProcessQuit::instance()->hup.connect(boost::bind(&Private::reload, this));
	}

	void start()
	{
		QCoreApplication::setApplicationName("pushpin");
		QCoreApplication::setApplicationVersion(Config::get().version);

		QCommandLineParser parser;
		parser.setApplicationDescription("Reverse proxy for realtime web services.");

		QString errorMessage;
		switch(parseCommandLine(&parser, &args, &errorMessage))
		{
			case CommandLineOk:
				break;
			case CommandLineError:
				fprintf(stderr, "error: %s\n\n%s", qPrintable(errorMessage), qPrintable(parser.helpText()));
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

		if(!args.logFile.isEmpty())
		{
			if(!log_setFile(args.logFile))
			{
				log_error("failed to open log file: %s", qPrintable(args.logFile));
				q->quit(1);
				return;
			}
		}

		QStringList configFileList;

		if(!args.configFile.isEmpty())
		{
			configFileList += args.configFile;
		}
		else
		{
			// ./config
			configFileList += QDir("config").absoluteFilePath("pushpin.conf");

			// same dir as executable (NOTE: deprecated)
			configFileList += QDir(".").absoluteFilePath("pushpin.conf");

			// ./examples/config
			configFileList += QDir("examples/config").absoluteFilePath("pushpin.conf");

			// default
			configFileList += QDir(Config::get().configDir).filePath("pushpin.conf");
		}

		QString configFile;

		foreach(const QString &f, configFileList)
		{
			if(QFileInfo(f).isFile())
			{
				configFile = f;
				break;
			}
		}

		if(configFile.isEmpty())
		{
			log_error("no configuration file found. Tried: %s", qPrintable(configFileList.join(" ")));
			q->quit(1);
			return;
		}

		// QSettings doesn't inform us if the config file can't be accessed,
		//   so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				log_error("failed to open %s", qPrintable(configFile));
				q->quit(1);
				return;
			}
		}

		int defaultArgsLevel = args.logLevels.value("", LOG_LEVEL_INFO);
		log_setOutputLevel(args.logLevels.value("runner", defaultArgsLevel));

		log_debug("starting...");

		if(args.configFile.isEmpty())
			log_info("using config: %s", qPrintable(configFile));

		Settings settings(configFile);

		QString exeDir = QCoreApplication::applicationDirPath();

		// NOTE: libdir in config file is deprecated
		QString libDir = settings.value("global/libdir").toString();

		if(!libDir.isEmpty())
		{
			libDir = QDir(libDir).absoluteFilePath("runner");
		}
		else
		{
			if(QFile::exists("src/bin/pushpin.rs"))
			{
				// running in tree
				libDir = QFileInfo("src/runner").absoluteFilePath();
			}
			else
			{
				// use compiled value
				libDir = QDir(Config::get().libDir).absoluteFilePath("runner");
			}
		}

		QString ipcPrefix = settings.value("global/ipc_prefix", "pushpin-").toString();

		QString configDir = QFileInfo(configFile).dir().filePath("runner");

		QStringList serviceNames = settings.value("runner/services").toStringList();
		trimlist(&serviceNames);

		QStringList httpPortStrs = settings.value("runner/http_port").toStringList();
		trimlist(&httpPortStrs);

		QStringList httpsPortStrs = settings.value("runner/https_ports").toStringList();
		trimlist(&httpsPortStrs);

		QStringList localPortStrs = settings.value("runner/local_ports").toStringList();
		trimlist(&localPortStrs);

		QString runDir;
		if(settings.contains("global/rundir"))
		{
			runDir = settings.value("global/rundir").toString();
		}
		else
		{
			log_warning("rundir in [runner] section is deprecated. put in [global]");
			runDir = settings.value("runner/rundir").toString();
		}

		runDir = QDir(runDir).absolutePath();

		QString logDir = settings.value("runner/logdir").toString();
		logDir = QDir(logDir).absolutePath();

		QMap<QString, int> logLevels;

		QStringList logLevelParts = settings.value("runner/log_level").toStringList();
		if(!logLevelParts.isEmpty())
		{
			logLevels = parseLogLevel(logLevelParts, &errorMessage);
			if(logLevels.isEmpty())
			{
				fprintf(stderr, "error: %s\n", qPrintable(errorMessage));
				q->quit(1);
				return;
			}
		}

		// command line overrides config file
		if(!args.logLevels.isEmpty())
			logLevels = args.logLevels;

		// if default log level not provided, use info level
		int defaultLevel = logLevels.value("", 2);

		// NOTE: since we only finally set the log level here, earlier
		//   log messages outside the default level will be lost (if any)
		log_setOutputLevel(logLevels.value("runner", defaultLevel));

		int clientBufferSize = settings.value("runner/client_buffer_size", 8192).toInt();
		int clientMaxConnections = settings.value("runner/client_maxconn", 50000).toInt();

		bool allowCompression = settings.value("runner/allow_compression").toBool();

		QString targetDir;
		if(ffi::is_debug_build())
			targetDir = QDir(exeDir).filePath("target/debug");
		else
			targetDir = QDir(exeDir).filePath("target/release");

		QString m2aBin = "m2adapter";
		QFileInfo fi(QDir(targetDir).filePath("m2adapter"));
		if(fi.isFile())
			m2aBin = fi.canonicalFilePath();

		QString connmgrBin = "pushpin-connmgr";
		fi = QFileInfo(QDir(targetDir).filePath("pushpin-connmgr"));
		if(fi.isFile())
			connmgrBin = fi.canonicalFilePath();

		QString proxyBin = "pushpin-proxy";
		fi = QFileInfo(QDir(targetDir).filePath("pushpin-proxy"));
		if(fi.isFile())
			proxyBin = fi.canonicalFilePath();

		QString handlerBin = "pushpin-handler";
		fi = QFileInfo(QDir(targetDir).filePath("pushpin-handler"));
		if(fi.isFile())
			handlerBin = fi.canonicalFilePath();

		if(!ensureDir(runDir))
		{
			log_error("failed to create directory: %s", qPrintable(runDir));
			q->quit(1);
			return;
		}

		if(!args.mergeOutput && !ensureDir(logDir))
		{
			log_error("failed to create directory: %s", qPrintable(logDir));
			q->quit(1);
			return;
		}

		int portOffset = 0;
		QString filePrefix;

		QList<ListenPort> ports;

		if(args.port.second > 0)
		{
			// if port specified then instantiate a single http server
			ports += ListenPort(args.port.first, args.port.second, false);
		}
		else
		{
			foreach(const QString &httpPortStr, httpPortStrs)
			{
				QPair<QHostAddress, int> p = parsePort(httpPortStr);
				if(p.second < 0)
				{
					log_error("invalid http port: %s", qPrintable(httpPortStr));
					q->quit(1);
					return;
				}

				ports += ListenPort(p.first, p.second, false);
			}

			foreach(const QString &httpsPortStr, httpsPortStrs)
			{
				QPair<QHostAddress, int> p = parsePort(httpsPortStr);
				if(p.second < 1)
				{
					log_error("invalid https port: %s", qPrintable(httpsPortStr));
					q->quit(1);
					return;
				}

				ports += ListenPort(p.first, p.second, true);
			}

			foreach(const QString &localPortStr, localPortStrs)
			{
				QUrl path = QUrl::fromEncoded(localPortStr.toUtf8());
				if(!path.isValid())
				{
					log_error("invalid local port: %s", qPrintable(localPortStr));
					q->quit(1);
					return;
				}

				QUrlQuery query(path.query());

				int mode = -1;
				if(query.hasQueryItem("mode"))
				{
					QString modeStr = query.queryItemValue("mode");
					bool ok = false;
					mode = modeStr.toInt(&ok, 8);
					if(!ok)
					{
						log_error("invalid mode: %s", qPrintable(modeStr));
						q->quit(1);
						return;
					}
				}

				QString user = query.queryItemValue("user");
				QString group = query.queryItemValue("group");

				ports += ListenPort(QHostAddress(), 0, true, path.path(), mode, user, group);
			}
		}

		if(ports.isEmpty())
		{
			log_error("no server ports configured");
			q->quit(1);
			return;
		}

		if(args.id >= 0)
		{
			ipcPrefix = QString("p%1-").arg(args.id);
			portOffset = args.id * 10;
			filePrefix = ipcPrefix;
		}

		if(logLevels.contains("pushpin-proxy"))
		{
			logLevels["proxy"] = logLevels["pushpin-proxy"];
			logLevels.remove("pushpin-proxy");
		}

		if(logLevels.contains("pushpin-handler"))
		{
			logLevels["handler"] = logLevels["pushpin-handler"];
			logLevels.remove("pushpin-handler");
		}

		if(serviceNames.contains("condure"))
		{
			serviceNames.removeAll("condure");
			serviceNames += "connmgr";
		}

		if(serviceNames.contains("pushpin-proxy"))
		{
			serviceNames.removeAll("pushpin-proxy");
			serviceNames += "proxy";
		}

		if(serviceNames.contains("pushpin-handler"))
		{
			serviceNames.removeAll("pushpin-handler");
			serviceNames += "handler";
		}

		if(serviceNames.contains("connmgr") && (serviceNames.contains("mongrel2") || serviceNames.contains("m2adapter")))
		{
			log_error("cannot enable the connmgr service at the same time as mongrel2 or m2adapter");
			q->quit(1);
			return;
		}

		if(serviceNames.contains("connmgr"))
		{
			QString certsDir = QDir(configDir).filePath("certs");

			bool useClient = false;

			if(!serviceNames.contains("zurl") && ConnmgrService::hasClientMode(connmgrBin))
				useClient = true;

			services += new ConnmgrService("connmgr", connmgrBin, runDir, !args.mergeOutput ? logDir : QString(), ipcPrefix, filePrefix, logLevels.value("connmgr", defaultLevel), certsDir, clientBufferSize, clientMaxConnections, allowCompression, ports, useClient);
		}

		if(serviceNames.contains("mongrel2"))
		{
			QString m2Bin = "mongrel2";
			if(settings.contains("runner/mongrel2_bin"))
				m2Bin = settings.value("runner/mongrel2_bin").toString();

			QString m2shBin = "m2sh";
			if(settings.contains("runner/m2sh_bin"))
				m2shBin = settings.value("runner/m2sh_bin").toString();

			QString certsDir = QDir(configDir).filePath("certs");
			if(!Mongrel2Service::generateConfigFile(m2shBin, QDir(libDir).filePath("mongrel2.conf.template"), runDir, !args.mergeOutput ? logDir : QString(), ipcPrefix, filePrefix, certsDir, clientBufferSize, clientMaxConnections, ports, logLevels.value("mongrel2", defaultLevel)))
			{
				q->quit(1);
				return;
			}

			foreach(const ListenPort &p, ports)
				services += new Mongrel2Service(m2Bin, QDir(runDir).filePath(QString("%1mongrel2.sqlite").arg(filePrefix)), "default_" + QString::number(p.port), runDir, !args.mergeOutput ? logDir : QString(), filePrefix, p.port, p.ssl, logLevels.value("mongrel2", defaultLevel));
		}

		if(serviceNames.contains("m2adapter"))
		{
			QList<int> portsOnly;
			foreach(const ListenPort &p, ports)
				portsOnly += p.port;

			services += new M2AdapterService(m2aBin, QDir(libDir).filePath("m2adapter.conf.template"), runDir, !args.mergeOutput ? logDir : QString(), ipcPrefix, filePrefix, logLevels.value("m2adapter", defaultLevel), portsOnly);
		}

		bool quietCheck = false;

		if(serviceNames.contains("zurl"))
		{
			QString zurlBin = "zurl";
			if(settings.contains("runner/zurl_bin"))
				zurlBin = settings.value("runner/zurl_bin").toString();

			services += new ZurlService(zurlBin, QDir(libDir).filePath("zurl.conf.template"), runDir, !args.mergeOutput ? logDir : QString(), ipcPrefix, filePrefix, logLevels.value("zurl", defaultLevel));

			// when zurl is managed by pushpin, log updates checks as debug level
			quietCheck = true;
		}

		if(serviceNames.contains("proxy"))
			services += new PushpinProxyService(proxyBin, configFile, runDir, !args.mergeOutput ? logDir : QString(), ipcPrefix, filePrefix, logLevels.value("proxy", defaultLevel), args.routeLines, quietCheck);

		if(serviceNames.contains("handler"))
			services += new PushpinHandlerService(handlerBin, configFile, runDir, !args.mergeOutput ? logDir : QString(), ipcPrefix, filePrefix, portOffset, logLevels.value("handler", defaultLevel));

		foreach(Service *s, services)
		{
			serviceConnectionMap[s] = {
				s->started.connect(boost::bind(&Private::service_started, this)),
				s->stopped.connect(boost::bind(&Private::service_stopped, this, s)),
				s->logLine.connect(boost::bind(&Private::service_logLine, this, boost::placeholders::_1, s)),
				s->error.connect(boost::bind(&Private::service_error, this, boost::placeholders::_1, s))
			};

			if(!args.mergeOutput || s->alwaysLogStatus())
				log_info("starting %s", qPrintable(s->name()));

			s->start();
		}
	}

private:
	QString tryInsertPrefix(const QString &line, const QString &prefix)
	{
		if(line.startsWith('['))
		{
			// find third space and insert the service name
			int at = 0;
			for(int n = 0; n < 3 && at != -1; ++n)
			{
				at = line.indexOf(' ', at);
				if(at != -1)
					++at;
			}

			if(at != -1)
			{
				QString out = line;
				out.insert(at, prefix);
				return out;
			}
		}

		return line;
	}

	void stopAll()
	{
		foreach(Service *s, services)
		{
			if(!args.mergeOutput || s->alwaysLogStatus())
				log_info("stopping %s", qPrintable(s->name()));

			s->stop();
		}
	}

	void checkStopped()
	{
		if(services.isEmpty())
		{
			log_debug("stopped");
			doQuit();
		}
	}

	void doQuit()
	{
		q->quit(errored ? 1 : 0);
	}

	void service_started()
	{
		bool allStarted = true;
		foreach(Service *s, services)
		{
			if(!s->isStarted())
			{
				allStarted = false;
				break;
			}
		}

		if(allStarted)
			log_debug("started");
	}

	void service_stopped(Service *s)
	{
		serviceConnectionMap.erase(s);

		services.removeAll(s);
		delete s;

		checkStopped();
	}

	void service_logLine(const QString &line, Service *s)
	{
		QString out = tryInsertPrefix(s->formatLogLine(line), '[' + s->name() + "] ");
		if(!out.isEmpty()) {
			log_raw(qPrintable(out));
		}
	}

	void service_error(const QString &error, Service *s)
	{
		log_error("%s: %s", qPrintable(s->name()), qPrintable(error));

		serviceConnectionMap.erase(s);

		services.removeAll(s);
		delete s;

		errored = true;

		if(stopping)
		{
			checkStopped();
		}
		else
		{
			// shutdown if we receive an unexpected error from any service
			stopping = true;
			stopAll();
		}
	}

	void reload()
	{
		log_info("reloading");
		log_rotate();

		foreach(Service *s, services)
		{
			if(s->acceptSighup())
				s->sendSighup();
		}
	}

	void processQuit()
	{
		if(!stopping)
		{
			// let a potential "^C" get overwritten
			printf("\r");

			stopping = true;
			ProcessQuit::reset(); // allow user to quit again

			log_info("stopping...");
			stopAll();
		}
		else
		{
			qDeleteAll(services);

			ProcessQuit::cleanup();

			// if we were already stopping, then exit immediately
			doQuit();
		}
	}
};

RunnerApp::RunnerApp() {
	d = std::make_unique<Private>(this);
}

RunnerApp::~RunnerApp() = default;

void RunnerApp::start()
{
	d->start();
}

