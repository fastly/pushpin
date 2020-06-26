/*
 * Copyright (C) 2013-2020 Fanout, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:AGPL$
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
 *
 * Alternatively, Pushpin may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#include "app.h"

#include <assert.h>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QPair>
#include <QHash>
#include <QDateTime>
#include <QTimer>
#include "qzmqsocket.h"
#include "qzmqvalve.h"
#include "processquit.h"
#include "tnetstring.h"
#include "m2requestpacket.h"
#include "m2responsepacket.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "bufferlist.h"
#include "log.h"
#include "layertracker.h"
#include "logutil.h"
#include "config.h"

#define DEFAULT_HWM 101000
#define STATUS_INTERVAL 250
#define REFRESH_INTERVAL 1000
#define M2_CONNECTION_EXPIRE 120000
#define ZHTTP_EXPIRE 60000
#define CONTROL_REQUEST_EXPIRE 30000
#define ZHTTP_CANCEL_RATE 100

#define M2_CONNECTION_SHOULD_PROCESS (M2_CONNECTION_EXPIRE * 3 / 4)
#define M2_CONNECTION_MUST_PROCESS (M2_CONNECTION_EXPIRE * 4 / 5)
#define M2_REFRESH_BUCKETS (M2_CONNECTION_SHOULD_PROCESS / REFRESH_INTERVAL)

#define ZHTTP_SHOULD_PROCESS (ZHTTP_EXPIRE * 3 / 4)
#define ZHTTP_MUST_PROCESS (ZHTTP_EXPIRE * 4 / 5)
#define ZHTTP_REFRESH_BUCKETS (ZHTTP_SHOULD_PROCESS / REFRESH_INTERVAL)

#define ZHTTP_CANCEL_PER_REFRESH (ZHTTP_CANCEL_RATE * 1000 / REFRESH_INTERVAL)

// make sure this is not larger than Mongrel2's DELIVER_OUTSTANDING_MSGS
#define M2_PENDING_MAX 16

// make sure this is not larger than Mongrel2's limits.handler_targets
#define M2_HANDLER_TARGETS_MAX 128

// this doesn't have to match the peer, but we'll set a reasonable number
#define ZHTTP_IDS_MAX 128

//#define CONTROL_PORT_DEBUG

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

static bool validateHost(const QByteArray &in)
{
	for(int n = 0; n < in.count(); ++n)
	{
		if(in[n] == '/')
			return false;
	}

	return true;
}

static QByteArray createResponseHeader(int code, const QByteArray &reason, const HttpHeaders &headers)
{
	QByteArray out = "HTTP/1.1 " + QByteArray::number(code) + ' ' + reason + "\r\n";
	foreach(const HttpHeader &h, headers)
		out += h.first + ": " + h.second + "\r\n";
	out += "\r\n";
	return out;
}

static QByteArray makeChunkHeader(int size)
{
	return QByteArray::number(size, 16).toUpper() + "\r\n";
}

static QByteArray makeChunkFooter()
{
	return "\r\n";
}

static bool isErrorPacket(const ZhttpResponsePacket &packet)
{
	return (packet.type == ZhttpResponsePacket::Error || packet.type == ZhttpResponsePacket::Cancel);
}

static void writeBigEndian(char *dest, quint64 value, int bytes)
{
	for(int n = 0; n < bytes; ++n)
		dest[n] = (char)((value >> ((bytes - 1 - n) * 8)) & 0xff);
}

static QByteArray makeWsHeader(bool fin, int opcode, quint64 size)
{
	quint8 b1 = 0;
	if(fin)
		b1 |= 0x80;
	b1 |= (opcode & 0x0f);

	if(size < 126)
	{
		QByteArray out(2, 0);
		out[0] = (char)b1;
		out[1] = (char)size;
		return out;
	}
	else if(size < 65536)
	{
		QByteArray out(4, 0);
		out[0] = (char)b1;
		out[1] = (char)126;
		writeBigEndian(out.data() + 2, size, 2);
		return out;
	}
	else
	{
		QByteArray out(10, 0);
		out[0] = (char)b1;
		out[1] = (char)127;
		writeBigEndian(out.data() + 2, size, 8);
		return out;
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

	ArgsData() :
		logLevel(-1)
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

	return CommandLineOk;
}

class App::Private : public QObject
{
	Q_OBJECT

public:
	enum Mode
	{
		Http,
		WebSocket
	};

	class ControlPort
	{
	public:
		enum State
		{
			Disabled,
			Idle,
			ExpectingResponse
		};

		QZmq::Socket *sock;
		State state;
		bool works;
		int reqStartTime;

		ControlPort() :
			sock(0),
			state(Disabled),
			works(false),
			reqStartTime(-1)
		{
		}
	};

	// can be used for either m2 or zhttp
	typedef QPair<QByteArray, QByteArray> Rid;

	class Session;

	class M2PendingOutItem
	{
	public:
		enum Type
		{
			Headers,
			Response,
			Frame,
			Close
		};

		Type type;
		BufferList data;
		bool chunked;
		int contentSize;

		M2PendingOutItem(Type _type) :
			type(_type),
			chunked(false),
			contentSize(0)
		{
		}
	};

	class M2Connection
	{
	public:
		int identIndex;
		QByteArray id;
		int confirmedBytesWritten;
		int packetsPending; // count of packets sent to m2 not yet ack'd
		Session *session;
		bool isNew;
		bool continuation;
		LayerTracker bodyTracker;
		LayerTracker packetTracker;
		QList<M2PendingOutItem> pendingOutItems; // packets yet to send
		bool flowControl;
		bool waitForAllWritten;
		bool outCreditsEnabled;
		int outCredits;
		quint64 subIdBase;
		qint64 lastRefresh;
		int refreshBucket;

		M2Connection() :
			confirmedBytesWritten(0),
			packetsPending(0),
			session(0),
			isNew(false),
			continuation(false),
			flowControl(false),
			waitForAllWritten(false),
			outCreditsEnabled(false),
			outCredits(0),
			subIdBase(0)
		{
		}

		bool canWrite() const
		{
			if(!flowControl)
				return true;

			if(outCreditsEnabled)
			{
				if(outCredits > 0)
					return true;
			}
			else
			{
				// if we aren't using outCredits, then limit pending packets
				//   to hardcoded m2 value
				if(packetsPending < M2_PENDING_MAX)
					return true;
			}

			return false;
		}
	};

	class Session
	{
	public:
		Mode mode;
		qint64 lastActive;
		QByteArray errorCondition;
		QByteArray acceptToken; // for websocket
		bool downClosed; // for websocket
		bool upClosed; // for websockets
		QString method;
		bool responseHeadersOnly; // HEAD, 204, 304
		qint64 lastRefresh;
		int refreshBucket;
		bool pendingCancel;

		// m2 stuff
		M2Connection *conn;
		bool persistent;
		bool allowChunked;
		bool respondKeepAlive;
		bool respondClose;
		bool chunked;
		int readCount;
		BufferList pendingIn;
		QList<ZhttpRequestPacket> pendingInPackets;
		bool inFinished;

		// zhttp stuff
		QByteArray id;
		QByteArray zhttpAddress;
		bool sentResponseHeader;
		int outSeq;
		int inSeq;
		int pendingInCredits;
		bool inHandoff;
		bool multi;

		Session() :
			lastActive(-1),
			downClosed(false),
			upClosed(false),
			responseHeadersOnly(false),
			lastRefresh(-1),
			pendingCancel(false),
			persistent(false),
			allowChunked(false),
			respondKeepAlive(false),
			respondClose(false),
			chunked(false),
			readCount(0),
			inFinished(false),
			sentResponseHeader(false),
			outSeq(0),
			inSeq(0),
			pendingInCredits(0),
			inHandoff(false),
			multi(false)
		{
		}
	};

	App *q;
	ArgsData args;
	QByteArray zhttpInstanceId;
	QByteArray zwsInstanceId;
	QZmq::Socket *m2_in_sock;
	QZmq::Socket *m2_out_sock;
	QZmq::Socket *zhttp_in_sock;
	QZmq::Socket *zhttp_out_sock;
	QZmq::Socket *zhttp_out_stream_sock;
	QZmq::Socket *zws_in_sock;
	QZmq::Socket *zws_out_sock;
	QZmq::Socket *zws_out_stream_sock;
	QZmq::Valve *m2_in_valve;
	QZmq::Valve *zhttp_in_valve;
	QZmq::Valve *zws_in_valve;
	QList<QByteArray> m2_send_idents;
	QHash<Rid, M2Connection*> m2ConnectionsByRid;
	QHash<Rid, Session*> sessionsByM2Rid;
	QHash<Rid, Session*> sessionsByZhttpRid;
	QHash<Rid, Session*> sessionsByZwsRid;
	QMap<QPair<qint64, M2Connection*>, M2Connection*> m2ConnectionsByLastRefresh;
	QSet<M2Connection*> m2ConnectionRefreshBuckets[M2_REFRESH_BUCKETS];
	int currentM2RefreshBucket;
	QMap<QPair<qint64, Session*>, Session*> sessionsByLastRefresh;
	QSet<Session*> sessionRefreshBuckets[ZHTTP_REFRESH_BUCKETS];
	int currentSessionRefreshBucket;
	QMap<QPair<qint64, Session*>, Session*> sessionsByLastActive;
	int zhttpCancelMeter;
	QSet<Session*> sessionsToCancel;
	int m2_client_buffer;
	int maxSessions;
	int zhttpConnectPort;
	int zwsConnectPort;
	bool ignorePolicies;
	QList<ControlPort> controlPorts;
	QTime time;
	QTimer *statusTimer;
	QTimer *refreshTimer;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		m2_in_sock(0),
		m2_out_sock(0),
		zhttp_in_sock(0),
		zhttp_out_sock(0),
		zhttp_out_stream_sock(0),
		zws_in_sock(0),
		zws_out_sock(0),
		zws_out_stream_sock(0),
		m2_in_valve(0),
		zhttp_in_valve(0),
		zws_in_valve(0),
		currentM2RefreshBucket(0),
		currentSessionRefreshBucket(0),
		zhttpCancelMeter(0)
	{
		connect(ProcessQuit::instance(), &ProcessQuit::quit, this, &Private::doQuit);
		connect(ProcessQuit::instance(), &ProcessQuit::hup, this, &Private::reload);

		statusTimer = new QTimer(this);
		connect(statusTimer, &QTimer::timeout, this, &Private::status_timeout);

		refreshTimer = new QTimer(this);
		connect(refreshTimer, &QTimer::timeout, this, &Private::refresh_timeout);
	}

	~Private()
	{
		qDeleteAll(sessionsByZhttpRid);
		qDeleteAll(sessionsByZwsRid);
		qDeleteAll(m2ConnectionsByRid);
	}

	void start()
	{
		QCoreApplication::setApplicationName("m2adapter");
		QCoreApplication::setApplicationVersion(VERSION);

		QCommandLineParser parser;
		parser.setApplicationDescription("Mongrel2 <-> ZHTTP adapter.");

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

		if(!init())
		{
			emit q->quit(1);
			return;
		}

		m2_in_valve->open();

		if(zhttp_in_valve)
			zhttp_in_valve->open();
		if(zws_in_valve)
			zws_in_valve->open();

		statusTimer->setInterval(STATUS_INTERVAL);

		refreshTimer->setInterval(REFRESH_INTERVAL);
		refreshTimer->start();

		log_info("started");
	}

	bool init()
	{
		if(args.logLevel != -1)
			log_setOutputLevel(args.logLevel);
		else
			log_setOutputLevel(LOG_LEVEL_INFO);

		if(!args.logFile.isEmpty())
		{
			if(!log_setFile(args.logFile))
			{
				log_error("failed to open log file: %s", qPrintable(args.logFile));
				return false;
			}
		}

		log_info("starting...");

		QString configFile = args.configFile;
		if(configFile.isEmpty())
			configFile = QDir(CONFIGDIR).filePath("m2adapter.conf");

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				log_error("failed to open %s, and --config not passed", qPrintable(configFile));
				return false;
			}
		}

		QSettings settings(configFile, QSettings::IniFormat);

		QStringList m2_in_specs = settings.value("m2_in_specs").toStringList();
		trimlist(&m2_in_specs);
		QStringList m2_out_specs = settings.value("m2_out_specs").toStringList();
		trimlist(&m2_out_specs);
		QStringList str_m2_send_idents = settings.value("m2_send_idents").toStringList();
		trimlist(&str_m2_send_idents);
		QStringList m2_control_specs = settings.value("m2_control_specs").toStringList();
		trimlist(&m2_control_specs);

		bool zhttp_connect = settings.value("zhttp_connect").toBool();
		QStringList zhttp_in_specs = settings.value("zhttp_in_specs").toStringList();
		trimlist(&zhttp_in_specs);
		QStringList zhttp_out_specs = settings.value("zhttp_out_specs").toStringList();
		trimlist(&zhttp_out_specs);
		QStringList zhttp_out_stream_specs = settings.value("zhttp_out_stream_specs").toStringList();
		trimlist(&zhttp_out_stream_specs);

		bool zws_connect = settings.value("zws_connect").toBool();
		QStringList zws_in_specs = settings.value("zws_in_specs").toStringList();
		trimlist(&zws_in_specs);
		QStringList zws_out_specs = settings.value("zws_out_specs").toStringList();
		trimlist(&zws_out_specs);
		QStringList zws_out_stream_specs = settings.value("zws_out_stream_specs").toStringList();
		trimlist(&zws_out_stream_specs);	

		zhttpConnectPort = settings.value("zhttp_connect_port", -1).toInt();
		zwsConnectPort = settings.value("zws_connect_port", -1).toInt();
		ignorePolicies = settings.value("ignore_policies").toBool();
		m2_client_buffer = settings.value("m2_client_buffer").toInt();
		if(m2_client_buffer <= 0)
			m2_client_buffer = 200000;

		maxSessions = settings.value("max_open_requests", -1).toInt();

		m2_send_idents.clear();
		foreach(const QString &s, str_m2_send_idents)
			m2_send_idents += s.toUtf8();

		if(m2_in_specs.isEmpty() || m2_out_specs.isEmpty() || m2_control_specs.isEmpty())
		{
			log_error("must set m2_in_specs, m2_out_specs, and m2_control_specs");
			return false;
		}

		if(m2_send_idents.count() != m2_control_specs.count())
		{
			log_error("m2_control_specs must have the same count as m2_send_idents");
			return false;
		}

		if((!zhttp_in_specs.isEmpty() || !zhttp_out_specs.isEmpty() || !zhttp_out_stream_specs.isEmpty()) && (zhttp_in_specs.isEmpty() || zhttp_out_specs.isEmpty() || zhttp_out_stream_specs.isEmpty()))
		{
			log_error("if zhttp is used, must set all of zhttp_in_specs, zhttp_out_specs, and zhttp_out_stream_specs");
			return false;
		}

		if((!zws_in_specs.isEmpty() || !zws_out_specs.isEmpty() || !zws_out_stream_specs.isEmpty()) && (zws_in_specs.isEmpty() || zws_out_specs.isEmpty() || zws_out_stream_specs.isEmpty()))
		{
			log_error("if zws is used, must set all of zws_in_specs, zws_out_specs, and zws_out_stream_specs");
			return false;
		}

		if(zhttp_in_specs.isEmpty() || zws_in_specs.isEmpty())
		{
			log_error("must set zhttp_* and/or zws_* specs");
			return false;
		}

		QByteArray pidStr = QByteArray::number(QCoreApplication::applicationPid());
		zhttpInstanceId = "m2zhttp_" + pidStr;
		zwsInstanceId = "m2zws_" + pidStr;

		m2_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
		m2_in_sock->setHwm(DEFAULT_HWM);
		foreach(const QString &spec, m2_in_specs)
		{
			log_info("m2_in connect %s", qPrintable(spec));
			m2_in_sock->connectToAddress(spec);
		}

		m2_in_valve = new QZmq::Valve(m2_in_sock, this);
		connect(m2_in_valve, &QZmq::Valve::readyRead, this, &Private::m2_in_readyRead);

		m2_out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
		m2_out_sock->setShutdownWaitTime(0);
		m2_out_sock->setHwm(DEFAULT_HWM);
		m2_out_sock->setWriteQueueEnabled(false);
		foreach(const QString &spec, m2_out_specs)
		{
			log_info("m2_out connect %s", qPrintable(spec));
			m2_out_sock->connectToAddress(spec);
		}

		for(int n = 0; n < m2_control_specs.count(); ++n)
		{
			const QString &spec = m2_control_specs[n];

			QZmq::Socket *sock = new QZmq::Socket(QZmq::Socket::Dealer, this);
			sock->setShutdownWaitTime(0);
			sock->setHwm(1); // queue up 1 outstanding request at most
			sock->setWriteQueueEnabled(false);
			connect(sock, &QZmq::Socket::readyRead, this, &Private::m2_control_readyRead);

			log_info("m2_control connect %s:%s", m2_send_idents[n].data(), qPrintable(spec));
			sock->connectToAddress(spec);

			ControlPort controlPort;
			controlPort.sock = sock;
			controlPorts += controlPort;
		}

		if(!zhttp_in_specs.isEmpty())
		{
			zhttp_in_sock = new QZmq::Socket(QZmq::Socket::Sub, this);
			zhttp_in_sock->setHwm(DEFAULT_HWM);
			zhttp_in_sock->setShutdownWaitTime(0);
			zhttp_in_sock->subscribe(zhttpInstanceId + ' ');
			if(zhttp_connect)
			{
				foreach(const QString &spec, zhttp_in_specs)
				{
					log_info("zhttp_in connect %s", qPrintable(spec));
					zhttp_in_sock->connectToAddress(spec);
				}
			}
			else
			{
				log_info("zhttp_in bind %s", qPrintable(zhttp_in_specs[0]));
				if(!zhttp_in_sock->bind(zhttp_in_specs[0]))
				{
					log_error("unable to bind to zhttp_in spec: %s", qPrintable(zhttp_in_specs[0]));
					return false;
				}
			}

			zhttp_in_valve = new QZmq::Valve(zhttp_in_sock, this);
			connect(zhttp_in_valve, &QZmq::Valve::readyRead, this, &Private::zhttp_in_readyRead);

			zhttp_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
			zhttp_out_sock->setShutdownWaitTime(0);
			zhttp_out_sock->setHwm(DEFAULT_HWM);
			if(zhttp_connect)
			{
				foreach(const QString &spec, zhttp_out_specs)
				{
					log_info("zhttp_out connect %s", qPrintable(spec));
					zhttp_out_sock->connectToAddress(spec);
				}
			}
			else
			{
				log_info("zhttp_out bind %s", qPrintable(zhttp_out_specs[0]));
				if(!zhttp_out_sock->bind(zhttp_out_specs[0]))
				{
					log_error("unable to bind to zhttp_out spec: %s", qPrintable(zhttp_out_specs[0]));
					return false;
				}
			}

			zhttp_out_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);
			zhttp_out_stream_sock->setShutdownWaitTime(0);
			zhttp_out_stream_sock->setHwm(DEFAULT_HWM);
			if(zhttp_connect)
			{
				foreach(const QString &spec, zhttp_out_stream_specs)
				{
					log_info("zhttp_out_stream connect %s", qPrintable(spec));
					zhttp_out_stream_sock->connectToAddress(spec);
				}
			}
			else
			{
				log_info("zhttp_out_stream bind %s", qPrintable(zhttp_out_stream_specs[0]));
				if(!zhttp_out_stream_sock->bind(zhttp_out_stream_specs[0]))
				{
					log_error("unable to bind to zhttp_out_stream spec: %s", qPrintable(zhttp_out_stream_specs[0]));
					return false;
				}
			}
		}

		if(!zws_in_specs.isEmpty())
		{
			zws_in_sock = new QZmq::Socket(QZmq::Socket::Sub, this);
			zws_in_sock->setHwm(DEFAULT_HWM);
			zws_in_sock->subscribe(zwsInstanceId + ' ');
			if(zws_connect)
			{
				foreach(const QString &spec, zws_in_specs)
				{
					log_info("zws_in connect %s", qPrintable(spec));
					zws_in_sock->connectToAddress(spec);
				}
			}
			else
			{
				log_info("zws_in bind %s", qPrintable(zws_in_specs[0]));
				if(!zws_in_sock->bind(zws_in_specs[0]))
				{
					log_error("unable to bind to zws_in spec: %s", qPrintable(zws_in_specs[0]));
					return false;
				}
			}

			zws_in_valve = new QZmq::Valve(zws_in_sock, this);
			connect(zws_in_valve, &QZmq::Valve::readyRead, this, &Private::zws_in_readyRead);

			zws_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
			zws_out_sock->setShutdownWaitTime(0);
			zws_out_sock->setHwm(DEFAULT_HWM);
			if(zws_connect)
			{
				foreach(const QString &spec, zws_out_specs)
				{
					log_info("zws_out connect %s", qPrintable(spec));
					zws_out_sock->connectToAddress(spec);
				}
			}
			else
			{
				log_info("zws_out bind %s", qPrintable(zws_out_specs[0]));
				if(!zws_out_sock->bind(zws_out_specs[0]))
				{
					log_error("unable to bind to zws_out spec: %s", qPrintable(zws_out_specs[0]));
					return false;
				}
			}

			zws_out_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);
			zws_out_stream_sock->setShutdownWaitTime(0);
			zws_out_stream_sock->setHwm(DEFAULT_HWM);
			if(zws_connect)
			{
				foreach(const QString &spec, zws_out_stream_specs)
				{
					log_info("zws_out_stream connect %s", qPrintable(spec));
					zws_out_stream_sock->connectToAddress(spec);
				}
			}
			else
			{
				log_info("zws_out_stream bind %s", qPrintable(zws_out_stream_specs[0]));
				if(!zws_out_stream_sock->bind(zws_out_stream_specs[0]))
				{
					log_error("unable to bind to zws_out_stream spec: %s", qPrintable(zws_out_stream_specs[0]));
					return false;
				}
			}
		}

		return true;
	}

	void removeConnection(M2Connection *conn)
	{
		m2ConnectionRefreshBuckets[conn->refreshBucket].remove(conn);
		m2ConnectionsByLastRefresh.remove(QPair<qint64, M2Connection*>(conn->lastRefresh, conn));
		m2ConnectionsByRid.remove(Rid(m2_send_idents[conn->identIndex], conn->id));
	}

	void unlinkConnection(Session *s)
	{
		if(s->conn)
		{
			s->conn->session = 0; // unlink the M2Connection so that it may be reused
			if(s->conn->packetsPending > 0 || !s->conn->pendingOutItems.isEmpty())
				s->conn->waitForAllWritten = true;
			sessionsByM2Rid.remove(Rid(m2_send_idents[s->conn->identIndex], s->conn->id));
			s->conn = 0;
		}
	}

	int smallestM2RefreshBucket()
	{
		int best = -1;
		int bestSize = 0;

		for(int n = 0; n < M2_REFRESH_BUCKETS; ++n)
		{
			if(best == -1 || m2ConnectionRefreshBuckets[n].count() < bestSize)
			{
				best = n;
				bestSize = m2ConnectionRefreshBuckets[n].count();
			}
		}

		return best;
	}

	int smallestSessionRefreshBucket()
	{
		int best = -1;
		int bestSize = 0;

		for(int n = 0; n < ZHTTP_REFRESH_BUCKETS; ++n)
		{
			if(best == -1 || sessionRefreshBuckets[n].count() < bestSize)
			{
				best = n;
				bestSize = sessionRefreshBuckets[n].count();
			}
		}

		return best;
	}

	void removeSession(Session *s)
	{
		unlinkConnection(s);

		sessionsToCancel -= s;

		if(s->lastRefresh >= 0)
		{
			QPair<qint64, Session*> k(s->lastRefresh, s);
			if(sessionsByLastRefresh.contains(k))
			{
				sessionRefreshBuckets[s->refreshBucket].remove(s);
				sessionsByLastRefresh.remove(k);
			}
		}

		sessionsByLastActive.remove(QPair<qint64, Session*>(s->lastActive, s));

		if(s->mode == Http)
			sessionsByZhttpRid.remove(Rid(zhttpInstanceId, s->id));
		else // WebSocket
			sessionsByZwsRid.remove(Rid(zwsInstanceId, s->id));
	}

	void destroySession(Session *s)
	{
		removeSession(s);
		delete s;
	}

	void queueCancelSession(Session *s)
	{
		assert(!s->zhttpAddress.isEmpty());

		unlinkConnection(s);

		s->pendingCancel = true;
		sessionsToCancel += s;
	}

	void destroySessionAndErrorConnection(Session *s)
	{
		M2Connection *conn = s->conn;
		destroySession(s);
		if(conn)
			m2_writeErrorClose(conn);
	}

	void m2_out_write(const M2ResponsePacket &packet)
	{
		QByteArray buf = packet.toByteArray();

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logByteArray(LOG_LEVEL_DEBUG, buf, "m2: OUT");

		m2_out_sock->write(QList<QByteArray>() << buf);
	}

	void m2_control_write(int index, const QByteArray &cmd, const QVariantHash &args)
	{
		QVariantList vlist;
		vlist += cmd;
		vlist += args;

		QByteArray buf = TnetString::fromVariant(vlist);

#ifdef CONTROL_PORT_DEBUG
		log_debug("m2: OUT control %s %s", m2_send_idents[index].data(), buf.data());
#endif

		QList<QByteArray> message;
		message += QByteArray();
		message += buf;
		controlPorts[index].sock->write(message);
	}

	void m2_writeCtl(M2Connection *conn, const QVariant &args)
	{
		M2ResponsePacket mresp;
		mresp.sender = m2_send_idents[conn->identIndex];
		mresp.id = "X " + conn->id;
		QVariantList parts;
		parts += QByteArray("ctl");
		parts += args;
		mresp.data = TnetString::fromVariant(parts);
		m2_out_write(mresp);
	}

	void m2_writeCtlMany(const QByteArray &sender, const QList<QByteArray> &connIds, const QVariant &args)
	{
		assert(!connIds.isEmpty());

		M2ResponsePacket mresp;
		mresp.sender = sender;
		mresp.id = "X";
		foreach(const QByteArray &id, connIds)
			mresp.id += " " + id;
		QVariantList parts;
		parts += QByteArray("ctl");
		parts += args;
		mresp.data = TnetString::fromVariant(parts);
		m2_out_write(mresp);
	}

	void m2_writeCtlCancel(const QByteArray &sender, const QByteArray &id)
	{
		M2ResponsePacket mresp;
		mresp.sender = sender;
		mresp.id = "X " + id;
		QVariantHash args;
		args["cancel"] = true;
		QVariantList parts;
		parts += QByteArray("ctl");
		parts += args;
		mresp.data = TnetString::fromVariant(parts);
		m2_out_write(mresp);
	}

	void m2_writeCtlCancel(M2Connection *conn)
	{
		m2_writeCtlCancel(m2_send_idents[conn->identIndex], conn->id);
		removeConnection(conn);
		delete conn;
	}

	// contentSize = packet.data.size() - framing overhead
	void m2_writeData(M2Connection *conn, const M2ResponsePacket &packet, int contentSize)
	{
		if(conn->outCreditsEnabled)
			conn->outCredits -= packet.data.size();

		conn->bodyTracker.addPlain(contentSize);
		conn->bodyTracker.specifyEncoded(packet.data.size(), contentSize);

		++(conn->packetsPending);
		conn->packetTracker.addPlain(1);
		conn->packetTracker.specifyEncoded(packet.data.size(), 1);

		m2_out_write(packet);
	}

	void m2_queueHeaders(M2Connection *conn, const QByteArray &headerData)
	{
		// only try writing if this item would be next
		bool tryWrite = conn->pendingOutItems.isEmpty();

		M2PendingOutItem item(M2PendingOutItem::Headers);
		item.data += headerData;
		conn->pendingOutItems += item;

		if(tryWrite)
			m2_tryWriteQueued(conn);
	}

	void m2_queueResponse(M2Connection *conn, const QByteArray &data, bool chunked)
	{
		// skip if the result would send no bytes
		if(data.isEmpty() && !chunked)
			return;

		// only try writing if this item would be next
		bool tryWrite = conn->pendingOutItems.isEmpty();

		M2PendingOutItem *item = 0;
		if(!conn->pendingOutItems.isEmpty())
		{
			M2PendingOutItem &last = conn->pendingOutItems.last();
			bool lastIsZeroChunk = (last.chunked && last.data.isEmpty());

			// see if we can merge with the previous item
			if(last.type == M2PendingOutItem::Response && last.chunked == chunked && !lastIsZeroChunk)
				item = &last;
		}
		if(!item)
		{
			conn->pendingOutItems += M2PendingOutItem(M2PendingOutItem::Response);
			item = &(conn->pendingOutItems.last());
		}

		item->data += data;
		item->chunked = chunked;

		if(tryWrite)
			m2_tryWriteQueued(conn);
	}

	void m2_queueFrame(M2Connection *conn, const QByteArray &data, int contentSize)
	{
		// only try writing if this item would be next
		bool tryWrite = conn->pendingOutItems.isEmpty();

		M2PendingOutItem item(M2PendingOutItem::Frame);
		item.data += data;
		item.contentSize = contentSize;
		conn->pendingOutItems += item;

		if(tryWrite)
			m2_tryWriteQueued(conn);
	}

	void m2_queueClose(M2Connection *conn)
	{
		// only try writing if this item would be next
		bool tryWrite = conn->pendingOutItems.isEmpty();

		conn->pendingOutItems += M2PendingOutItem(M2PendingOutItem::Close);

		if(tryWrite)
			m2_tryWriteQueued(conn);
	}

	// return true if connection was deleted as a result of writing queued items
	bool m2_tryWriteQueued(M2Connection *conn)
	{
		while(!conn->pendingOutItems.isEmpty() && conn->canWrite())
		{
			M2PendingOutItem *item = &conn->pendingOutItems.first();
			if(item->type == M2PendingOutItem::Headers)
			{
				M2ResponsePacket packet;
				packet.sender = m2_send_idents[conn->identIndex];
				packet.id = conn->id;
				packet.data = item->data.take();

				conn->pendingOutItems.removeFirst();

				m2_writeData(conn, packet, 0);

				if(!conn->flowControl)
					handleConnectionBytesWritten(conn, packet.data.size(), true);
			}
			else if(item->type == M2PendingOutItem::Response)
			{
				// only write what we're allowed to
				int maxSize;
				if(conn->outCreditsEnabled)
				{
					if(item->chunked)
						maxSize = conn->outCredits - 16; // make room for chunked header
					else
						maxSize = conn->outCredits;
				}
				else
				{
					maxSize = 200000; // some reasonable max
				}

				if(maxSize <= 0)
				{
					// can't write at this time
					break;
				}

				QByteArray data = item->data.take(maxSize);
				int contentSize = data.size();

				M2ResponsePacket packet;
				packet.sender = m2_send_idents[conn->identIndex];
				packet.id = conn->id;

				if(item->chunked)
					packet.data = makeChunkHeader(data.size()) + data + makeChunkFooter();
				else
					packet.data = data;

				if(item->data.isEmpty())
					conn->pendingOutItems.removeFirst();

				m2_writeData(conn, packet, contentSize);

				if(!conn->flowControl)
					handleConnectionBytesWritten(conn, packet.data.size(), true);
			}
			else if(item->type == M2PendingOutItem::Frame)
			{
				M2ResponsePacket packet;
				packet.sender = m2_send_idents[conn->identIndex];
				packet.id = conn->id;
				packet.data = item->data.take();

				int contentSize = item->contentSize;

				conn->pendingOutItems.removeFirst();

				m2_writeData(conn, packet, contentSize);

				if(!conn->flowControl)
					handleConnectionBytesWritten(conn, packet.data.size(), true);
			}
			else if(item->type == M2PendingOutItem::Close)
			{
				conn->pendingOutItems.removeFirst();

				m2_writeClose(conn); // this will delete the connection
				return true;
			}
		}

		return false;
	}

	void m2_writeClose(const QByteArray &sender, const QByteArray &id)
	{
		M2ResponsePacket mresp;
		mresp.sender = sender;
		mresp.id = id;
		mresp.data = "";
		m2_out_write(mresp);
	}

	void m2_writeClose(M2Connection *conn)
	{
		m2_writeClose(m2_send_idents[conn->identIndex], conn->id);
		removeConnection(conn);
		delete conn;
	}

	void m2_writeErrorClose(const QByteArray &sender, const QByteArray &id)
	{
		// same as closing. in the future we may want to send something interesting first.
		m2_writeClose(sender, id);
	}

	void m2_writeErrorClose(M2Connection *conn)
	{
		// same as closing. in the future we may want to send something interesting first.
		m2_writeClose(conn);
	}

	void zhttp_out_write(Mode mode, const ZhttpRequestPacket &packet)
	{
		const char *logprefix = (mode == Http ? "zhttp" : "zws");

		QVariant vpacket = packet.toVariant();
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s: OUT", logprefix);

		if(mode == Http)
			zhttp_out_sock->write(QList<QByteArray>() << buf);
		else // WebSocket
			zws_out_sock->write(QList<QByteArray>() << buf);
	}

	void zhttp_out_write(Mode mode, const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
	{
		const char *logprefix = (mode == Http ? "zhttp" : "zws");

		QVariant vpacket = packet.toVariant();
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(vpacket);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, vpacket, "body", "%s: OUT instance=%s", logprefix, instanceAddress.data());

		QList<QByteArray> message;
		message += instanceAddress;
		message += QByteArray();
		message += buf;

		if(mode == Http)
			zhttp_out_stream_sock->write(message);
		else // WebSocket
			zws_out_stream_sock->write(message);
	}

	void zhttp_out_writeFirst(Session *s, const ZhttpRequestPacket &packet)
	{
		ZhttpRequestPacket out = packet;
		out.from = (s->mode == Http ? zhttpInstanceId : zwsInstanceId);
		out.ids += ZhttpRequestPacket::Id(s->id, (s->outSeq)++);
		zhttp_out_write(s->mode, out);
	}

	void zhttp_out_write(Session *s, const ZhttpRequestPacket &packet)
	{
		assert(!s->zhttpAddress.isEmpty());

		ZhttpRequestPacket out = packet;
		out.from = (s->mode == Http ? zhttpInstanceId : zwsInstanceId);
		out.ids += ZhttpRequestPacket::Id(s->id, (s->outSeq)++);
		zhttp_out_write(s->mode, out, s->zhttpAddress);
	}

	void handleControlResponse(int index, const QVariant &data)
	{
#ifdef CONTROL_PORT_DEBUG
		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			log_debug("m2: IN control %s %s", m2_send_idents[index].data(), qPrintable(TnetString::variantToString(data)));
#endif

		if(data.type() != QVariant::Hash)
			return;

		QVariantHash vhash = data.toHash();

		if(!vhash.contains("rows"))
			return;

		QVariant rows = vhash["rows"];

		// once we get at least one successful response then we flag the port as working
		if(!controlPorts[index].works)
		{
			controlPorts[index].works = true;
			log_debug("control port index=%d works", index);
		}

		QSet<QByteArray> ids;
		foreach(const QVariant &row, rows.toList())
		{
			if(row.type() != QVariant::List)
				break;

			QVariantList vlist = row.toList();
			QByteArray id = vlist[0].toByteArray();
			int bytes_written = vlist[7].toInt();

			ids += id;

			M2Connection *conn = m2ConnectionsByRid.value(Rid(m2_send_idents[index], id));
			if(!conn || !conn->flowControl || conn->outCreditsEnabled)
				continue;

			if(bytes_written > conn->confirmedBytesWritten)
			{
				int written = bytes_written - conn->confirmedBytesWritten;
				conn->confirmedBytesWritten = bytes_written;

				handleConnectionBytesWritten(conn, written, true);

				// if we had any pending writes to make, now's the time.
				// note: this might delete the connection but that's fine
				m2_tryWriteQueued(conn);
			}
		}

		// any connections missing?
		QList<M2Connection*> gone;
		QHashIterator<Rid, M2Connection*> it(m2ConnectionsByRid);
		while(it.hasNext())
		{
			it.next();
			M2Connection *conn = it.value();
			if(conn->identIndex == index)
			{
				// only check for missing connections that aren't flagged
				if(!conn->isNew)
				{
					if(!ids.contains(conn->id))
						gone += conn;
				}
				else
				{
					// clear the flag so the connection gets processed next time
					conn->isNew = false;
				}
			}
		}
		foreach(M2Connection *conn, gone)
		{
			log_debug("m2: %s id=%s disconnected", m2_send_idents[conn->identIndex].data(), conn->id.data());

			if(conn->session)
				endSession(conn->session, "disconnected");

			removeConnection(conn);
			delete conn;
		}
	}

	// return true if connection was deleted as a result of handling bytes written
	void handleConnectionBytesWritten(M2Connection *conn, int written, bool giveCredits)
	{
		int bodyWritten = conn->bodyTracker.finished(written);
		int packetsWritten = conn->packetTracker.finished(written);

		conn->packetsPending -= packetsWritten;

		if(conn->waitForAllWritten && conn->packetsPending == 0 && conn->pendingOutItems.isEmpty())
			conn->waitForAllWritten = false;

		if(conn->session && bodyWritten > 0)
		{
			Session *s = conn->session;

			// update lastActive
			qint64 now = QDateTime::currentMSecsSinceEpoch();
			sessionsByLastActive.remove(QPair<qint64, Session*>(s->lastActive, s));
			s->lastActive = now;
			sessionsByLastActive.insert(QPair<qint64, Session*>(s->lastActive, s), s);

			handleSessionBodyWritten(s, bodyWritten, giveCredits);
		}
	}

	void handleSessionBodyWritten(Session *s, int written, bool giveCredits)
	{
		s->pendingInCredits += written;

		log_debug("request id=%s written %d%s", s->id.data(), written, s->conn->flowControl ? "" : " (no flow control)");

		if(s->inHandoff)
			return;

		// address could be empty here if we're handling write of non-sequenced response
		if(giveCredits && !s->zhttpAddress.isEmpty())
		{
			ZhttpRequestPacket zreq;
			zreq.type = ZhttpRequestPacket::Credit;
			zreq.credits = s->pendingInCredits;
			s->pendingInCredits = 0;
			zhttp_out_write(s, zreq);
		}
	}

	void endSession(Session *s, const QByteArray &errorCondition = QByteArray())
	{
		// if we are in handoff or haven't received a worker ack, then queue the state
		if(s->inHandoff || s->zhttpAddress.isEmpty())
		{
			if(!errorCondition.isEmpty())
				s->errorCondition = errorCondition;

			// keep the session around
			unlinkConnection(s);
		}
		else
		{
			if(sessionsToCancel.isEmpty() && zhttpCancelMeter < ZHTTP_CANCEL_PER_REFRESH)
			{
				++zhttpCancelMeter;

				ZhttpRequestPacket zreq;

				if(!errorCondition.isEmpty())
				{
					zreq.type = ZhttpRequestPacket::Error;
					zreq.condition = "disconnected";
				}
				else
					zreq.type = ZhttpRequestPacket::Cancel;

				zhttp_out_write(s, zreq);

				destroySession(s);
			}
			else
			{
				queueCancelSession(s);
			}
		}
	}

	void handleZhttpIn(Mode mode, const QList<QByteArray> &message)
	{
		const char *logprefix = (mode == Http ? "zhttp" : "zws");

		if(message.count() != 1)
		{
			log_warning("%s: received message with parts != 1, skipping", logprefix);
			return;
		}

		int at = message[0].indexOf(' ');
		if(at == -1)
		{
			log_warning("%s: received message with invalid format, skipping", logprefix);
			return;
		}

		QByteArray dataRaw = message[0].mid(at + 1);
		if(dataRaw.length() < 1 || dataRaw[0] != 'T')
		{
			log_warning("%s: received message with invalid format (missing type), skipping", logprefix);
			return;
		}

		QVariant data = TnetString::toVariant(dataRaw.mid(1));
		if(data.isNull())
		{
			log_warning("%s: received message with invalid format (tnetstring parse failed), skipping", logprefix);
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logVariantWithContent(LOG_LEVEL_DEBUG, data, "body", "%s: IN", logprefix);

		ZhttpResponsePacket zresp;
		if(!zresp.fromVariant(data))
		{
			log_warning("%s: received message with invalid format (parse failed), skipping", logprefix);
			return;
		}

		foreach(const ZhttpResponsePacket::Id &id, zresp.ids)
			handleZhttpIn(logprefix, mode, id.id, id.seq, zresp);
	}

	void handleZhttpIn(const char *logprefix, Mode mode, const QByteArray &id, int seq, const ZhttpResponsePacket &zresp)
	{
		Session *s;
		if(mode == Http)
			s = sessionsByZhttpRid.value(Rid(zhttpInstanceId, id));
		else // WebSocket
			s = sessionsByZwsRid.value(Rid(zwsInstanceId, id));

		if(!s)
		{
			log_debug("%s: received message for unknown request id, canceling", logprefix);

			// if this was not an error packet, send cancel
			if(!isErrorPacket(zresp) && !zresp.from.isEmpty())
			{
				ZhttpRequestPacket zreq;
				zreq.from = (mode == Http ? zhttpInstanceId : zwsInstanceId);
				zreq.ids += ZhttpRequestPacket::Id(id);
				zreq.type = ZhttpRequestPacket::Cancel;
				zhttp_out_write(mode, zreq, zresp.from);
			}

			return;
		}

		// mode will always match here
		assert(s->mode == mode);

		if(s->inSeq == 0)
		{
			// are we expecting a sequence of packets after the first?
			if((!isErrorPacket(zresp) && zresp.type != ZhttpResponsePacket::Data) || (zresp.type == ZhttpResponsePacket::Data && zresp.more))
			{
				// sequence must have from address
				if(zresp.from.isEmpty())
				{
					log_warning("%s: received first response of sequence with no from address, canceling", logprefix);
					destroySessionAndErrorConnection(s);
					return;
				}

				s->zhttpAddress = zresp.from;

				if(seq != 0)
				{
					log_warning("%s: received first response of sequence without valid seq, canceling", logprefix);
					ZhttpRequestPacket zreq;
					zreq.type = ZhttpRequestPacket::Cancel;
					zhttp_out_write(s, zreq);
					destroySessionAndErrorConnection(s);
					return;
				}
			}
			else
			{
				// if not sequenced, then there might be a from address
				if(!zresp.from.isEmpty())
					s->zhttpAddress = zresp.from;

				// if not sequenced, but seq is provided, then it must be 0
				if(seq != -1 && seq != 0)
				{
					log_warning("%s: received response out of sequence (got=%d, expected=-1,0), canceling", logprefix, seq);

					if(!s->zhttpAddress.isEmpty())
					{
						ZhttpRequestPacket zreq;
						zreq.type = ZhttpRequestPacket::Cancel;
						zhttp_out_write(s, zreq);
					}

					destroySessionAndErrorConnection(s);
					return;
				}
			}
		}
		else
		{
			if(seq != -1 && seq != s->inSeq)
			{
				log_warning("%s: received response out of sequence (got=%d, expected=%d), canceling", logprefix, seq, s->inSeq);
				ZhttpRequestPacket zreq;
				zreq.type = ZhttpRequestPacket::Cancel;
				zhttp_out_write(s, zreq);
				destroySessionAndErrorConnection(s);
				return;
			}

			// if a new from address is provided, update our copy
			if(!zresp.from.isEmpty())
				s->zhttpAddress = zresp.from;
		}

		// only bump sequence if seq was provided
		if(seq != -1)
			++(s->inSeq);

		qint64 now = QDateTime::currentMSecsSinceEpoch();

		if(s->lastRefresh < 0 && !s->zhttpAddress.isEmpty())
		{
			// once we have the peer's address, set up refresh

			s->lastRefresh = now;
			sessionsByLastRefresh.insert(QPair<qint64, Session*>(s->lastRefresh, s), s);

			s->refreshBucket = smallestSessionRefreshBucket();
			sessionRefreshBuckets[s->refreshBucket] += s;
		}

		// update lastActive
		sessionsByLastActive.remove(QPair<qint64, Session*>(s->lastActive, s));
		s->lastActive = now;
		sessionsByLastActive.insert(QPair<qint64, Session*>(s->lastActive, s), s);

		if(s->pendingCancel)
			return;

		// a session without a connection is just waiting to report error
		if(!s->conn)
		{
			// if we were in handoff, it's okay to send right now since we'd
			//   be clearing the handoff state later on in this method anyway
			if(!s->zhttpAddress.isEmpty())
			{
				ZhttpRequestPacket zreq;
				if(!s->errorCondition.isEmpty())
				{
					zreq.type = ZhttpRequestPacket::Error;
					zreq.condition = s->errorCondition;
				}
				else
					zreq.type = ZhttpRequestPacket::Cancel;
				zhttp_out_write(s, zreq);
			}

			destroySession(s);
			return;
		}

		// if peer supports multi feature then flag it on the session
		bool multiWasTurnedOn = false;
		if(!s->multi && zresp.multi)
		{
			s->multi = true;
			multiWasTurnedOn = true;
		}

		if(s->inHandoff)
		{
			// receiving any message means handoff is complete
			s->inHandoff = false;

			// refresh would have already been set up once if we are here
			assert(s->lastRefresh >= 0);

			sessionsByLastRefresh.insert(QPair<qint64, Session*>(s->lastRefresh, s), s);
			s->refreshBucket = smallestSessionRefreshBucket();
			sessionRefreshBuckets[s->refreshBucket] += s;

			// in order to have been in a handoff state, we would have
			//   had to receive a from address sometime earlier, so it
			//   should be safe to call zhttp_out_write with session.

			if(multiWasTurnedOn)
			{
				// acknowledge the feature
				ZhttpRequestPacket zreq;
				zreq.type = ZhttpRequestPacket::KeepAlive;
				zreq.multi = true;
				zhttp_out_write(s, zreq);
			}

			if(s->mode == Http)
			{
				if(!s->pendingIn.isEmpty())
				{
					ZhttpRequestPacket zreq;
					zreq.type = ZhttpRequestPacket::Data;

					// send credits too, if needed (though this probably can't happen,
					//   since http data flows only in one direction at a time. we
					//   can't have pending request body data while at the same
					//   time be acking received response body data).
					if(s->pendingInCredits > 0)
					{
						zreq.credits = s->pendingInCredits;
						s->pendingInCredits = 0;
					}

					zreq.body = s->pendingIn.take();
					zreq.more = !s->inFinished;
					zhttp_out_write(s, zreq);
				}
			}
			else // WebSocket
			{
				while(!s->pendingInPackets.isEmpty())
				{
					ZhttpRequestPacket zreq = s->pendingInPackets.takeFirst();

					// send credits too, if needed
					if(zreq.type == ZhttpRequestPacket::Data && s->pendingInCredits > 0)
					{
						zreq.credits = s->pendingInCredits;
						s->pendingInCredits = 0;
					}

					zhttp_out_write(s, zreq);
				}
			}

			// if we didn't send credits as part of a data packet, we'll do them now
			if(s->pendingInCredits > 0)
			{
				ZhttpRequestPacket zreq;
				zreq.type = ZhttpRequestPacket::Credit;
				zreq.credits = s->pendingInCredits;
				s->pendingInCredits = 0;
				zhttp_out_write(s, zreq);
			}
		}

		if(zresp.type == ZhttpResponsePacket::Data)
		{
			log_debug("zhttp: id=%s response data size=%d%s", s->id.data(), zresp.body.size(), zresp.more ? " M" : "");

			// data packet may have credits
			if(zresp.credits > 0)
			{
				QVariantHash args;
				args["credits"] = zresp.credits;
				m2_writeCtl(s->conn, args);
			}

			if(s->mode == Http)
			{
				bool firstDataPacket = !s->sentResponseHeader;

				// respond with data if we have body data or this is the first packet
				if(!zresp.body.isEmpty() || firstDataPacket)
				{
					if(firstDataPacket)
					{
						// use flow control if the control port works and the response is more than one packet
						if((s->conn->outCreditsEnabled || controlPorts[s->conn->identIndex].works) && zresp.more)
							s->conn->flowControl = true;
						else
							s->conn->flowControl = false;

						s->sentResponseHeader = true;

						if(zresp.more && !zresp.headers.contains("Content-Length"))
						{
							if(s->allowChunked)
							{
								s->chunked = true;
							}
							else
							{
								// disable persistence
								s->persistent = false;
								s->respondKeepAlive = false;
							}
						}

						if(s->method == "HEAD" || (zresp.code == 204 || zresp.code == 304))
							s->responseHeadersOnly = true;

						HttpHeaders headers = zresp.headers;
						QList<QByteArray> connHeaders = headers.takeAll("Connection");
						foreach(const QByteArray &h, connHeaders)
							headers.removeAll(h);

						headers.removeAll("Transfer-Encoding");

						connHeaders.clear();
						if(s->respondKeepAlive)
							connHeaders += "Keep-Alive";
						if(s->respondClose)
							connHeaders += "close";

						if(!s->responseHeadersOnly)
						{
							if(s->chunked)
							{
								connHeaders += "Transfer-Encoding";
								headers += HttpHeader("Transfer-Encoding", "chunked");
							}
							else if(!zresp.more && !headers.contains("Content-Length"))
							{
								headers += HttpHeader("Content-Length", QByteArray::number(zresp.body.size()));
							}
						}

						if(!connHeaders.isEmpty())
							headers += HttpHeader("Connection", HttpHeaders::join(connHeaders));

						log_info("OUT %s id=%s code=%d %d%s", m2_send_idents[s->conn->identIndex].data(), s->conn->id.data(), zresp.code, zresp.body.size(), zresp.more ? " M": "");

						m2_queueHeaders(s->conn, createResponseHeader(zresp.code, zresp.reason, headers));
					}

					if(!zresp.body.isEmpty())
					{
						if(s->responseHeadersOnly)
						{
							log_warning("%s: received unexpected response body, canceling", logprefix);

							bool persistent = s->persistent;

							// cancel and destroy session
							M2Connection *conn = s->conn;
							if(!s->zhttpAddress.isEmpty())
							{
								ZhttpRequestPacket zreq;
								zreq.type = ZhttpRequestPacket::Cancel;
								zhttp_out_write(s, zreq);
							}
							destroySession(s);
							if(!persistent)
								m2_queueClose(conn);
							return;
						}

						m2_queueResponse(s->conn, zresp.body, s->chunked);
					}

					if(!zresp.more && s->chunked)
					{
						// send closing chunk
						m2_queueResponse(s->conn, QByteArray(), true);
					}
				}
				else
				{
					if(!zresp.more && s->chunked)
					{
						// send closing chunk
						m2_queueResponse(s->conn, QByteArray(), true);
					}
				}

				if(!zresp.more)
				{
					bool persistent = s->persistent;
					M2Connection *conn = s->conn;
					destroySession(s);
					if(!persistent)
						m2_queueClose(conn);
				}
			}
			else // WebSocket
			{
				if(!s->sentResponseHeader)
				{
					s->sentResponseHeader = true;
					if(s->conn->outCreditsEnabled || controlPorts[s->conn->identIndex].works)
						s->conn->flowControl = true;
					else
						s->conn->flowControl = false;

					HttpHeaders headers = zresp.headers;
					QList<QByteArray> connHeaders = headers.takeAll("Connection");
					foreach(const QByteArray &h, connHeaders)
						headers.removeAll(h);
					headers.removeAll("Transfer-Encoding");
					headers.removeAll("Upgrade");
					headers.removeAll("Sec-WebSocket-Accept");

					headers += HttpHeader("Upgrade", "websocket");
					headers += HttpHeader("Connection", "Upgrade");
					headers += HttpHeader("Sec-WebSocket-Accept", s->acceptToken);

					QByteArray reason;
					if(!zresp.reason.isEmpty())
						reason = zresp.reason;
					else
						reason = "Switching Protocols";

					log_info("OUT %s id=%s code=%d 0 M", m2_send_idents[s->conn->identIndex].data(), s->conn->id.data(), zresp.code);

					m2_queueHeaders(s->conn, createResponseHeader(101, reason, headers));
				}
				else
				{
					int opcode;
					if(s->conn->continuation)
					{
						opcode = 0;
					}
					else
					{
						if(zresp.contentType == "binary")
							opcode = 2;
						else // text
							opcode = 1;
					}

					s->conn->continuation = zresp.more;

					QByteArray frame = makeWsHeader(!zresp.more, opcode, zresp.body.size()) + zresp.body;

					m2_queueFrame(s->conn, frame, zresp.body.size());
				}
			}
		}
		else if(zresp.type == ZhttpResponsePacket::Error)
		{
			log_debug("%s: id=%s error condition=%s", logprefix, s->id.data(), zresp.condition.data());

			if(s->mode == WebSocket && zresp.condition == "rejected")
			{
				HttpHeaders headers = zresp.headers;
				QList<QByteArray> connHeaders = headers.takeAll("Connection");
				foreach(const QByteArray &h, connHeaders)
					headers.removeAll(h);

				headers.removeAll("Transfer-Encoding");

				if(!headers.contains("Content-Length"))
					headers += HttpHeader("Content-Length", QByteArray::number(zresp.body.size()));

				connHeaders.clear();

				// if HTTP/1.1, include "Connection: close"
				if(s->allowChunked)
					connHeaders += "close";

				if(!connHeaders.isEmpty())
					headers += HttpHeader("Connection", HttpHeaders::join(connHeaders));

				log_info("OUT %s id=%s code=%d %d", m2_send_idents[s->conn->identIndex].data(), s->conn->id.data(), zresp.code, zresp.body.size());

				m2_queueHeaders(s->conn, createResponseHeader(zresp.code, zresp.reason, headers));
				m2_queueResponse(s->conn, zresp.body, false);

				M2Connection *conn = s->conn;
				destroySession(s);
				m2_queueClose(conn);
			}
			else
				destroySessionAndErrorConnection(s);
		}
		else if(zresp.type == ZhttpResponsePacket::Credit)
		{
			if(zresp.credits > 0)
			{
				QVariantHash args;
				args["credits"] = zresp.credits;
				m2_writeCtl(s->conn, args);
			}
		}
		else if(zresp.type == ZhttpResponsePacket::KeepAlive)
		{
			// nothing to do
		}
		else if(zresp.type == ZhttpResponsePacket::Cancel)
		{
			destroySessionAndErrorConnection(s);
		}
		else if(zresp.type == ZhttpResponsePacket::HandoffStart)
		{
			s->inHandoff = true;

			sessionRefreshBuckets[s->refreshBucket].remove(s);
			sessionsByLastRefresh.remove(QPair<qint64, Session*>(s->lastRefresh, s));

			// whoever picks up after handoff can turn this on
			s->multi = false;

			ZhttpRequestPacket zreq;
			zreq.type = ZhttpRequestPacket::HandoffProceed;
			zhttp_out_write(s, zreq);
		}
		else if(zresp.type == ZhttpResponsePacket::Close || zresp.type == ZhttpResponsePacket::Ping || zresp.type == ZhttpResponsePacket::Pong)
		{
			int opcode;
			if(zresp.type == ZhttpResponsePacket::Close)
			{
				opcode = 8;
				s->upClosed = true;
			}
			else if(zresp.type == ZhttpResponsePacket::Ping)
				opcode = 9;
			else // Pong
				opcode = 10;

			QByteArray data;
			if(zresp.type == ZhttpResponsePacket::Close)
			{
				data.resize(2 + zresp.body.size());
				writeBigEndian(data.data(), zresp.code != -1 ? zresp.code : 1000, 2);
				if(!zresp.body.isEmpty())
				{
					memcpy(data.data() + 2, zresp.body.data(), zresp.body.size());
				}
			} else {
				data = zresp.body;
			}

			QByteArray frame = makeWsHeader(true, opcode, data.size()) + data;

			m2_queueFrame(s->conn, frame, data.size());

			if(s->downClosed && s->upClosed)
			{
				M2Connection *conn = s->conn;
				destroySession(s);
				m2_queueClose(conn);
			}
		}
		else
		{
			log_warning("%s: id=%s unsupported type: %d", logprefix, s->id.data(), (int)zresp.type);
		}
	}

	void refreshM2Connections(qint64 now)
	{
		QHash<int, QList<QByteArray> > connIdListBySender;

		// process the current bucket
		const QSet<M2Connection*> &bucket = m2ConnectionRefreshBuckets[currentM2RefreshBucket];
		foreach(M2Connection *conn, bucket)
		{
			// move to the end
			QPair<qint64, M2Connection*> k(conn->lastRefresh, conn);
			m2ConnectionsByLastRefresh.remove(k);
			conn->lastRefresh = now;
			m2ConnectionsByLastRefresh.insert(QPair<qint64, M2Connection*>(conn->lastRefresh, conn), conn);

			if(!connIdListBySender.contains(conn->identIndex))
				connIdListBySender.insert(conn->identIndex, QList<QByteArray>());

			QList<QByteArray> &connIdList = connIdListBySender[conn->identIndex];
			connIdList += conn->id;

			// if we're at max, send out now
			if(connIdList.count() >= M2_HANDLER_TARGETS_MAX)
			{
				QVariantHash args;
				args["keep-alive"] = true;
				m2_writeCtlMany(m2_send_idents[conn->identIndex], connIdList, args);

				connIdList.clear();
				connIdListBySender.remove(conn->identIndex);
			}
		}

		// process any others
		qint64 threshold = now - M2_CONNECTION_MUST_PROCESS;
		while(!m2ConnectionsByLastRefresh.isEmpty())
		{
			QMap<QPair<qint64, M2Connection*>, M2Connection*>::iterator it = m2ConnectionsByLastRefresh.begin();
			M2Connection *conn = it.value();

			if(conn->lastRefresh > threshold)
				break;

			// move to the end
			m2ConnectionsByLastRefresh.erase(it);
			conn->lastRefresh = now;
			m2ConnectionsByLastRefresh.insert(QPair<qint64, M2Connection*>(conn->lastRefresh, conn), conn);

			if(!connIdListBySender.contains(conn->identIndex))
				connIdListBySender.insert(conn->identIndex, QList<QByteArray>());

			QList<QByteArray> &connIdList = connIdListBySender[conn->identIndex];
			connIdList += conn->id;

			// if we're at max, send out now
			if(connIdList.count() >= M2_HANDLER_TARGETS_MAX)
			{
				QVariantHash args;
				args["keep-alive"] = true;
				m2_writeCtlMany(m2_send_idents[conn->identIndex], connIdList, args);

				connIdList.clear();
				connIdListBySender.remove(conn->identIndex);
			}
		}

		// send last packet
		QHashIterator<int, QList<QByteArray> > cit(connIdListBySender);
		while(cit.hasNext())
		{
			cit.next();
			int index = cit.key();
			const QList<QByteArray> &connIdList = cit.value();

			if(!connIdList.isEmpty())
			{
				QVariantHash args;
				args["keep-alive"] = true;
				m2_writeCtlMany(m2_send_idents[index], connIdList, args);
			}
		}

		++currentM2RefreshBucket;
		if(currentM2RefreshBucket >= M2_REFRESH_BUCKETS)
			currentM2RefreshBucket = 0;
	}

	void refreshSessions(qint64 now)
	{
		QHash<QByteArray, QList<Session*> > sessionListBySender[2]; // index corresponds to mode

		// process the current bucket
		const QSet<Session*> &bucket = sessionRefreshBuckets[currentSessionRefreshBucket];
		foreach(Session *s, bucket)
		{
			assert(!s->inHandoff && !s->zhttpAddress.isEmpty());

			// move to the end
			QPair<qint64, Session*> k(s->lastRefresh, s);
			sessionsByLastRefresh.remove(k);
			s->lastRefresh = now;
			sessionsByLastRefresh.insert(QPair<qint64, Session*>(s->lastRefresh, s), s);

			if(s->multi)
			{
				if(!sessionListBySender[s->mode].contains(s->zhttpAddress))
					sessionListBySender[s->mode].insert(s->zhttpAddress, QList<Session*>());

				QList<Session*> &sessionList = sessionListBySender[s->mode][s->zhttpAddress];
				sessionList += s;

				// if we're at max, send out now
				if(sessionList.count() >= ZHTTP_IDS_MAX)
				{
					ZhttpRequestPacket zreq;
					zreq.from = (s->mode == Http ? zhttpInstanceId : zwsInstanceId);
					foreach(Session *i, sessionList)
						zreq.ids += ZhttpRequestPacket::Id(i->id, (i->outSeq)++);
					zreq.type = ZhttpRequestPacket::KeepAlive;
					zhttp_out_write(s->mode, zreq, s->zhttpAddress);

					sessionList.clear();
					sessionListBySender[s->mode].remove(s->zhttpAddress);
				}
			}
			else
			{
				// session doesn't support sending with multiple ids
				ZhttpRequestPacket zreq;
				zreq.from = (s->mode == Http ? zhttpInstanceId : zwsInstanceId);
				zreq.ids += ZhttpRequestPacket::Id(s->id, (s->outSeq)++);
				zreq.type = ZhttpRequestPacket::KeepAlive;
				zhttp_out_write(s->mode, zreq, s->zhttpAddress);
			}
		}

		// process any others
		qint64 threshold = now - ZHTTP_MUST_PROCESS;
		while(!sessionsByLastRefresh.isEmpty())
		{
			QMap<QPair<qint64, Session*>, Session*>::iterator it = sessionsByLastRefresh.begin();
			Session *s = it.value();

			if(s->lastRefresh > threshold)
				break;

			assert(!s->inHandoff && !s->zhttpAddress.isEmpty());

			// move to the end
			sessionsByLastRefresh.erase(it);
			s->lastRefresh = now;
			sessionsByLastRefresh.insert(QPair<qint64, Session*>(s->lastRefresh, s), s);

			if(s->multi)
			{
				if(!sessionListBySender[s->mode].contains(s->zhttpAddress))
					sessionListBySender[s->mode].insert(s->zhttpAddress, QList<Session*>());

				QList<Session*> &sessionList = sessionListBySender[s->mode][s->zhttpAddress];
				sessionList += s;

				// if we're at max, send out now
				if(sessionList.count() >= ZHTTP_IDS_MAX)
				{
					ZhttpRequestPacket zreq;
					zreq.from = (s->mode == Http ? zhttpInstanceId : zwsInstanceId);
					foreach(Session *i, sessionList)
						zreq.ids += ZhttpRequestPacket::Id(i->id, (i->outSeq)++);
					zreq.type = ZhttpRequestPacket::KeepAlive;
					zhttp_out_write(s->mode, zreq, s->zhttpAddress);

					sessionList.clear();
					sessionListBySender[s->mode].remove(s->zhttpAddress);
				}
			}
			else
			{
				// session doesn't support sending with multiple ids
				ZhttpRequestPacket zreq;
				zreq.from = (s->mode == Http ? zhttpInstanceId : zwsInstanceId);
				zreq.ids += ZhttpRequestPacket::Id(s->id, (s->outSeq)++);
				zreq.type = ZhttpRequestPacket::KeepAlive;
				zhttp_out_write(s->mode, zreq, s->zhttpAddress);
			}
		}

		// send last packets
		for(int n = 0; n < 2; ++n)
		{
			Mode mode = (Mode)n;

			QHashIterator<QByteArray, QList<Session*> > sit(sessionListBySender[n]);
			while(sit.hasNext())
			{
				sit.next();
				const QByteArray &zhttpAddress = sit.key();
				const QList<Session*> &sessionList = sit.value();

				if(!sessionList.isEmpty())
				{
					ZhttpRequestPacket zreq;
					zreq.from = (mode == Http ? zhttpInstanceId : zwsInstanceId);
					foreach(Session *s, sessionList)
						zreq.ids += ZhttpRequestPacket::Id(s->id, (s->outSeq)++);
					zreq.type = ZhttpRequestPacket::KeepAlive;
					zhttp_out_write(mode, zreq, zhttpAddress);
				}
			}
		}

		++currentSessionRefreshBucket;
		if(currentSessionRefreshBucket >= ZHTTP_REFRESH_BUCKETS)
			currentSessionRefreshBucket = 0;
	}

	void expireSessions(qint64 now)
	{
		qint64 threshold = now - ZHTTP_EXPIRE;
		while(!sessionsByLastActive.isEmpty())
		{
			QMap<QPair<qint64, Session*>, Session*>::iterator it = sessionsByLastActive.begin();
			Session *s = it.value();

			if(s->lastActive > threshold)
				break;

			log_warning("timing out request %s", s->id.data());
			destroySessionAndErrorConnection(s);
		}
	}

	void cancelSessions()
	{
		int sent = zhttpCancelMeter;

		if(zhttpCancelMeter > ZHTTP_CANCEL_PER_REFRESH)
			zhttpCancelMeter -= ZHTTP_CANCEL_PER_REFRESH;
		else
			zhttpCancelMeter = 0;

		QHash<QByteArray, QList<Session*> > sessionListBySender[2]; // index corresponds to mode

		while(!sessionsToCancel.isEmpty() && sent < ZHTTP_CANCEL_PER_REFRESH)
		{
			QSet<Session*>::iterator it = sessionsToCancel.begin();
			Session *s = (*it);
			sessionsToCancel.erase(it);

			if(s->multi)
			{
				if(!sessionListBySender[s->mode].contains(s->zhttpAddress))
					sessionListBySender[s->mode].insert(s->zhttpAddress, QList<Session*>());

				QList<Session*> &sessionList = sessionListBySender[s->mode][s->zhttpAddress];
				sessionList += s;

				// if we're at max, send out now
				if(sessionList.count() >= ZHTTP_IDS_MAX)
				{
					ZhttpRequestPacket zreq;
					zreq.from = (s->mode == Http ? zhttpInstanceId : zwsInstanceId);
					foreach(Session *i, sessionList)
						zreq.ids += ZhttpRequestPacket::Id(i->id, (i->outSeq)++);
					zreq.type = ZhttpRequestPacket::Cancel;
					zhttp_out_write(s->mode, zreq, s->zhttpAddress);

					Mode mode = s->mode;
					QByteArray zhttpAddress = s->zhttpAddress;

					foreach(Session *s, sessionList)
						destroySession(s);

					sessionList.clear();
					sessionListBySender[mode].remove(zhttpAddress);
				}
			}
			else
			{
				// session doesn't support sending with multiple ids
				ZhttpRequestPacket zreq;
				zreq.from = (s->mode == Http ? zhttpInstanceId : zwsInstanceId);
				zreq.ids += ZhttpRequestPacket::Id(s->id, (s->outSeq)++);
				zreq.type = ZhttpRequestPacket::Cancel;
				zhttp_out_write(s->mode, zreq, s->zhttpAddress);

				destroySession(s);
			}

			++sent;
		}

		// send last packets
		for(int n = 0; n < 2; ++n)
		{
			Mode mode = (Mode)n;

			QHashIterator<QByteArray, QList<Session*> > sit(sessionListBySender[n]);
			while(sit.hasNext())
			{
				sit.next();
				const QByteArray &zhttpAddress = sit.key();
				const QList<Session*> &sessionList = sit.value();

				if(!sessionList.isEmpty())
				{
					ZhttpRequestPacket zreq;
					zreq.from = (mode == Http ? zhttpInstanceId : zwsInstanceId);
					foreach(Session *s, sessionList)
						zreq.ids += ZhttpRequestPacket::Id(s->id, (s->outSeq)++);
					zreq.type = ZhttpRequestPacket::Cancel;
					zhttp_out_write(mode, zreq, zhttpAddress);
				}

				foreach(Session *s, sessionList)
					destroySession(s);
			}
		}
	}

private slots:
	void m2_in_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("m2: received message with parts != 1, skipping");
			return;
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			LogUtil::logByteArray(LOG_LEVEL_DEBUG, message[0], "m2: IN");

		M2RequestPacket mreq;
		if(!mreq.fromByteArray(message[0]))
		{
			log_warning("m2: received message with invalid format, skipping");
			return;
		}

		if(mreq.type == M2RequestPacket::Disconnect)
		{
			log_debug("m2: %s id=%s disconnected", mreq.sender.data(), mreq.id.data());

			Rid rid(mreq.sender, mreq.id);

			M2Connection *conn = m2ConnectionsByRid.value(rid);
			if(!conn)
				return;

			if(conn->session)
				endSession(conn->session);

			removeConnection(conn);
			delete conn;

			return;
		}

		qint64 now = QDateTime::currentMSecsSinceEpoch();

		Rid m2Rid(mreq.sender, mreq.id);

		M2Connection *conn = m2ConnectionsByRid.value(m2Rid);
		if(!conn)
		{
			if(mreq.version.isEmpty())
			{
				if(mreq.type == M2RequestPacket::HttpRequest || mreq.type == M2RequestPacket::WebSocketHandshake) {
					log_warning("m2: id=%s no version on initial packet", mreq.id.data());
				}

				m2_writeCtlCancel(mreq.sender, mreq.id);
				return;
			}

			if(mreq.version != "HTTP/1.0" && mreq.version != "HTTP/1.1")
			{
				log_error("m2: id=%s unknown version: %s", mreq.id.data(), mreq.version.data());
				m2_writeCtlCancel(mreq.sender, mreq.id);
				return;
			}

			int index = -1;
			for(int n = 0; n < m2_send_idents.count(); ++n)
			{
				if(m2_send_idents[n] == mreq.sender)
				{
					index = n;
					break;
				}
			}

			if(index == -1)
			{
				log_error("m2: id=%s unknown send_ident [%s]", mreq.id.data(), mreq.sender.data());
				return;
			}

			if(sessionsByM2Rid.contains(m2Rid))
			{
				log_warning("m2: received duplicate request id=%s, skipping", mreq.id.data());
				m2_writeCtlCancel(mreq.sender, mreq.id);
				return;
			}

			conn = new M2Connection;
			conn->identIndex = index;
			conn->id = mreq.id;

			if(mreq.downloadCredits >= 0)
			{
				conn->outCreditsEnabled = true;
				conn->outCredits += mreq.downloadCredits;
			}
			else
			{
				if(controlPorts[index].state == ControlPort::Disabled)
				{
					log_debug("activating control port index=%d", index);
					controlPorts[index].state = ControlPort::Idle;

					if(!statusTimer->isActive())
						statusTimer->start();
				}

				// if we were in the middle of requesting control info when this
				//   http request arrived, then there's a chance the control
				//   response won't account for this request (for example if the
				//   control response was generated and was in the middle of being
				//   delivered when this http request arrived). we'll flag the
				//   connection as "new" in this case, so in the control response
				//   handler we know to skip over it until the next control
				//   request.
				if(controlPorts[index].state == ControlPort::ExpectingResponse)
					conn->isNew = true;
			}

			m2ConnectionsByRid.insert(m2Rid, conn);

			conn->lastRefresh = now;
			m2ConnectionsByLastRefresh.insert(QPair<qint64, M2Connection*>(conn->lastRefresh, conn), conn);

			conn->refreshBucket = smallestM2RefreshBucket();
			m2ConnectionRefreshBuckets[conn->refreshBucket] += conn;
		}
		else
		{
			// if packet contained credits, handle them now
			if(conn->outCreditsEnabled && mreq.downloadCredits > 0)
			{
				conn->outCredits += mreq.downloadCredits;
				handleConnectionBytesWritten(conn, mreq.downloadCredits, true);

				// if we had any pending writes to make, now's the time
				bool connDeleted = m2_tryWriteQueued(conn);
				if(connDeleted)
					return;
			}

			// if the packet only held credits, then there's nothing else to do
			if(mreq.type == M2RequestPacket::Credits)
				return;
		}

		bool requestBodyMore = false;
		if(mreq.type == M2RequestPacket::HttpRequest && mreq.uploadStreamOffset >= 0 && !mreq.uploadStreamDone)
			requestBodyMore = true;

		Session *s = sessionsByM2Rid.value(m2Rid);
		if(!s)
		{
			if(mreq.type == M2RequestPacket::HttpRequest && mreq.uploadStreamOffset > 0)
			{
				log_warning("m2: id=%s stream offset > 0 but session unknown", mreq.id.data());
				m2_writeCtlCancel(conn);
				return;
			}

			if(mreq.type != M2RequestPacket::HttpRequest && mreq.type != M2RequestPacket::WebSocketHandshake)
			{
				log_warning("m2: received unexpected starting packet type: %d", (int)mreq.type);
				m2_writeCtlCancel(conn);
				return;
			}

			QByteArray scheme;
			if(mreq.type == M2RequestPacket::HttpRequest)
			{
				if(mreq.scheme == "https")
					scheme = "https";
				else
					scheme = "http";
			}
			else // WebSocketHandshake
			{
				if(mreq.scheme == "https" || mreq.scheme == "wss")
					scheme = "wss";
				else
					scheme = "ws";
			}

			QByteArray host = mreq.headers.get("Host");
			if(host.isEmpty())
				host = "localhost";

			if(!validateHost(host))
			{
				log_warning("m2: invalid host [%s]", host.data());
				m2_writeErrorClose(conn);
				return;
			}

			if(!mreq.uri.startsWith('/'))
			{
				log_warning("m2: invalid uri [%s]", mreq.uri.data());
				m2_writeErrorClose(conn);
				return;
			}

			QByteArray uriRaw = scheme + "://" + host + mreq.uri;
			QUrl uri = QUrl::fromEncoded(uriRaw, QUrl::TolerantMode);
			if(!uri.isValid())
			{
				log_warning("m2: invalid constructed uri: [%s]", uriRaw.data());
				m2_writeErrorClose(conn);
				return;
			}

			if(maxSessions >= 0 && sessionsByZhttpRid.count() + sessionsByZwsRid.count() >= maxSessions)
			{
				log_warning("m2: max open sessions reached (%d), refusing new session", maxSessions);
				m2_writeErrorClose(conn);
				return;
			}

			s = new Session;
			s->conn = conn;
			s->conn->session = s;
			s->id = m2_send_idents[conn->identIndex] + '_' + conn->id + '_' + QByteArray::number((conn->subIdBase)++, 16);
			s->method = mreq.method;

			if(mreq.type == M2RequestPacket::HttpRequest)
			{
				s->mode = Http;

				if(mreq.version == "HTTP/1.0")
				{
					if(mreq.headers.getAll("Connection").contains("Keep-Alive"))
					{
						s->persistent = true;
						s->respondKeepAlive = true;
					}
				}
				else if(mreq.version == "HTTP/1.1")
				{
					s->allowChunked = true;

					if(mreq.headers.getAll("Connection").contains("close"))
						s->respondClose = true;
					else
						s->persistent = true;
				}

				s->readCount += mreq.body.size();

				if(!requestBodyMore)
					s->inFinished = true;
			}
			else // WebSocketHandshake
			{
				s->mode = WebSocket;
				s->acceptToken = mreq.body;
			}

			sessionsByM2Rid.insert(m2Rid, s);

			qint64 now = QDateTime::currentMSecsSinceEpoch();

			s->lastActive = now;
			sessionsByLastActive.insert(QPair<qint64, Session*>(s->lastActive, s), s);

			if(mreq.type == M2RequestPacket::HttpRequest)
				sessionsByZhttpRid.insert(Rid(zhttpInstanceId, s->id), s);
			else // WebSocketHandshake
				sessionsByZwsRid.insert(Rid(zwsInstanceId, s->id), s);

			log_info("IN %s id=%s %s %s", m2_send_idents[s->conn->identIndex].data(), s->conn->id.data(), s->mode == Http ? qPrintable(mreq.method) : "GET", uri.toEncoded().data());

			ZhttpRequestPacket zreq;

			zreq.type = ZhttpRequestPacket::Data;
			if(conn->outCreditsEnabled)
				zreq.credits = conn->outCredits;
			else
				zreq.credits = m2_client_buffer;
			zreq.uri = uri;
			zreq.headers = mreq.headers;
			zreq.peerAddress = mreq.remoteAddress;
			if(mreq.type == M2RequestPacket::HttpRequest && zhttpConnectPort != -1)
				zreq.connectPort = zhttpConnectPort;
			else if(zwsConnectPort != -1) // WebSocketHandshake
				zreq.connectPort = zwsConnectPort;
			if(ignorePolicies)
				zreq.ignorePolicies = true;

			if(mreq.type == M2RequestPacket::HttpRequest)
			{
				zreq.stream = true;
				zreq.method = mreq.method;
				zreq.body = mreq.body;
				zreq.more = !s->inFinished;
			}

			zreq.multi = true;

			zhttp_out_writeFirst(s, zreq);
		}
		else
		{
			assert(s->conn == conn);

			if(mreq.type != M2RequestPacket::HttpRequest && mreq.type != M2RequestPacket::WebSocketFrame)
			{
				log_warning("m2: received unexpected subsequent packet type: %d", (int)mreq.type);
				endSession(s);
				m2_writeCtlCancel(conn);
				return;
			}

			if(mreq.type == M2RequestPacket::HttpRequest)
			{
				int offset = 0;
				if(mreq.uploadStreamOffset > 0)
					offset = mreq.uploadStreamOffset;

				if(offset != s->readCount)
				{
					log_warning("m2: %s id=%s unexpected stream offset (got=%d, expected=%d)", m2_send_idents[s->conn->identIndex].data(), mreq.id.data(), offset, s->readCount);
					endSession(s);
					m2_writeCtlCancel(conn);
					return;
				}

				s->readCount += mreq.body.size();

				if(!requestBodyMore)
					s->inFinished = true;
			}

			if(s->zhttpAddress.isEmpty())
			{
				log_error("m2: %s id=%s multiple packets from m2 before response from zhttp", m2_send_idents[s->conn->identIndex].data(), mreq.id.data());
				endSession(s);
				m2_writeCtlCancel(conn);
				return;
			}

			if(mreq.type == M2RequestPacket::HttpRequest)
			{
				if(s->inHandoff)
				{
					s->pendingIn += mreq.body;
				}
				else
				{
					ZhttpRequestPacket zreq;
					zreq.type = ZhttpRequestPacket::Data;
					zreq.body = mreq.body;
					zreq.more = !s->inFinished;
					zhttp_out_write(s, zreq);
				}
			}
			else // WebSocketFrame
			{
				int opcode = mreq.frameFlags & 0x0f;
				if(opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10)
				{
					log_warning("m2: %s id=%s unsupported ws opcode: %d", m2_send_idents[s->conn->identIndex].data(), mreq.id.data(), opcode);
					endSession(s);
					m2_writeCtlCancel(conn);
					return;
				}

				if(s->downClosed)
				{
					log_debug("m2: %s id=%s ignoring frame after close", m2_send_idents[s->conn->identIndex].data(), mreq.id.data());
					return;
				}

				ZhttpRequestPacket zreq;

				if(opcode == 1 || opcode == 2)
				{
					zreq.type = ZhttpRequestPacket::Data;
					if(opcode == 2)
						zreq.contentType = "binary";
					zreq.body = mreq.body;
				}
				else if(opcode == 8)
				{
					zreq.type = ZhttpRequestPacket::Close;
					if(mreq.body.size() >= 2)
					{
						int hi = (unsigned char)mreq.body[0];
						int lo = (unsigned char)mreq.body[1];
						zreq.code = (hi << 8) + lo;
						zreq.body = mreq.body.mid(2);
					}

					s->downClosed = true;
				}
				else if(opcode == 9)
				{
					zreq.type = ZhttpRequestPacket::Ping;
					zreq.body = mreq.body;
				}
				else // 10
				{
					zreq.type = ZhttpRequestPacket::Pong;
					zreq.body = mreq.body;
				}

				if(s->inHandoff)
				{
					s->pendingInPackets += zreq;
				}
				else
				{
					zhttp_out_write(s, zreq);

					if(s->downClosed && s->upClosed)
					{
						destroySession(s); // we aren't in handoff so this is safe
						m2_queueClose(conn);
					}
				}
			}
		}
	}

	void m2_control_readyRead()
	{
		QZmq::Socket *sock = (QZmq::Socket *)sender();
		int index = -1;
		for(int n = 0; n < controlPorts.count(); ++n)
		{
			if(controlPorts[n].sock == sock)
			{
				index = n;
				break;
			}
		}

		assert(index != -1);
		ControlPort &c = controlPorts[index];

		while(sock->canRead())
		{
			QList<QByteArray> message = sock->read();

			if(message.count() != 2)
			{
				log_warning("m2: received control response with parts != 2, skipping");
				continue;
			}

			QVariant data = TnetString::toVariant(message[1]);
			if(data.isNull())
			{
				log_warning("m2: received control response with invalid format (tnetstring parse failed), skipping");
				continue;
			}

			if(c.state != ControlPort::ExpectingResponse)
			{
				log_warning("m2: received unexpected control response, skipping");
				continue;
			}

			handleControlResponse(index, data);

			bool needControlPort = false;
			QHashIterator<Rid, M2Connection*> it(m2ConnectionsByRid);
			while(it.hasNext())
			{
				it.next();
				M2Connection *conn = it.value();
				if(!conn->outCreditsEnabled)
					needControlPort = true;
			}

			if(needControlPort)
			{
				c.state = ControlPort::Idle;
			}
			else
			{
				log_debug("deactivating control port index=%d", index);
				c.state = ControlPort::Disabled;

				bool allDisabled = true;
				foreach(const ControlPort &i, controlPorts)
				{
					if(i.state != ControlPort::Disabled)
					{
						allDisabled = false;
						break;
					}
				}
				if(allDisabled)
					statusTimer->stop();
			}

			c.reqStartTime = -1;
		}
	}

	void zhttp_in_readyRead(const QList<QByteArray> &message)
	{
		handleZhttpIn(Http, message);
	}

	void zws_in_readyRead(const QList<QByteArray> &message)
	{
		handleZhttpIn(WebSocket, message);
	}

	void status_timeout()
	{
		int now = time.elapsed();

		for(int n = 0; n < controlPorts.count(); ++n)
		{
			ControlPort &c = controlPorts[n];

			if(c.state == ControlPort::Disabled)
				continue;

			// if idle or expired, make request
			if(c.state == ControlPort::Idle || (c.state == ControlPort::ExpectingResponse && c.reqStartTime + CONTROL_REQUEST_EXPIRE <= now))
			{
				// query m2 for connection info (to track bytes written)
				QVariantHash cmdArgs;
				cmdArgs["what"] = QByteArray("net");
				c.state = ControlPort::ExpectingResponse;
				c.reqStartTime = now;
				m2_control_write(n, "status", cmdArgs);
			}
		}
	}

	void refresh_timeout()
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();

		refreshM2Connections(now);
		refreshSessions(now);
		expireSessions(now);
		cancelSessions();
	}

	void reload()
	{
		log_info("reloading");
		log_rotate();
	}

	void doQuit()
	{
		log_info("stopping...");

		// remove the handler, so if we get another signal then we crash out
		ProcessQuit::cleanup();

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
