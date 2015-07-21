/*
 * Copyright (C) 2013-2014 Fanout, Inc.
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
#include <QPair>
#include <QHash>
#include <QTime>
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

#define VERSION "1.4.0"

#define DEFAULT_HWM 51000
#define EXPIRE_INTERVAL 1000
#define STATUS_INTERVAL 250
#define M2_KEEPALIVE_INTERVAL 90000
#define SESSION_EXPIRE 60000
#define CONTROL_REQUEST_EXPIRE 30000

// make sure this is not larger than Mongrel2's DELIVER_OUTSTANDING_MSGS
#define M2_PENDING_MAX 16

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
			Idle,
			ExpectingResponse
		};

		QZmq::Socket *sock;
		State state;
		bool active;
		int reqStartTime;

		ControlPort() :
			sock(0),
			state(Idle),
			active(false),
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
		QByteArray sender;
		QByteArray id;
		BufferList data;
		int bodySize;

		M2PendingOutItem(const QByteArray &_sender, const QByteArray &_id) :
			sender(_sender),
			id(_id),
			bodySize(0)
		{
		}
	};

	class M2Connection
	{
	public:
		int identIndex;
		QByteArray id;
		int confirmedBytesWritten;
		int packetsPending;
		Session *session;
		bool isNew;
		LayerTracker bodyTracker;
		LayerTracker packetTracker;
		QList<M2PendingOutItem> pendingOutItems;
		bool flowControl;
		bool waitForAllWritten;

		M2Connection() :
			confirmedBytesWritten(0),
			packetsPending(0),
			session(0),
			isNew(false),
			flowControl(false),
			waitForAllWritten(false)
		{
		}

		bool canWrite() const
		{
			return (!flowControl || packetsPending < M2_PENDING_MAX);
		}
	};

	class Session
	{
	public:
		Mode mode;
		int lastActive;
		QByteArray errorCondition;
		QByteArray acceptToken; // for websocket
		bool downClosed; // for websocket
		bool upClosed; // for websockets
		QString method;
		bool responseHeadersOnly; // HEAD, 204, 304

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

		Session() :
			lastActive(-1),
			downClosed(false),
			upClosed(false),
			responseHeadersOnly(false),
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
			inHandoff(false)
		{
		}
	};

	App *q;
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
	int m2_client_buffer;
	int zhttpConnectPort;
	int zwsConnectPort;
	bool ignorePolicies;
	QList<ControlPort> controlPorts;
	QTime time;
	QTimer *expireTimer;
	QTimer *statusTimer;
	QTimer *keepAliveTimer;
	QTimer *m2KeepAliveTimer;

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
		zws_in_valve(0)
	{
		connect(ProcessQuit::instance(), SIGNAL(quit()), SLOT(doQuit()));
		connect(ProcessQuit::instance(), SIGNAL(hup()), SLOT(reload()));

		time.start();

		expireTimer = new QTimer(this);
		connect(expireTimer, SIGNAL(timeout()), SLOT(expire_timeout()));

		statusTimer = new QTimer(this);
		connect(statusTimer, SIGNAL(timeout()), SLOT(status_timeout()));

		keepAliveTimer = new QTimer(this);
		connect(keepAliveTimer, SIGNAL(timeout()), SLOT(keepAlive_timeout()));

		m2KeepAliveTimer = new QTimer(this);
		connect(m2KeepAliveTimer, SIGNAL(timeout()), SLOT(m2KeepAlive_timeout()));
	}

	~Private()
	{
		qDeleteAll(sessionsByM2Rid);
		qDeleteAll(m2ConnectionsByRid);
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

		if(!init(options))
		{
			emit q->quit();
			return;
		}

		m2_in_valve->open();

		if(zhttp_in_valve)
			zhttp_in_valve->open();
		if(zws_in_valve)
			zws_in_valve->open();

		expireTimer->setInterval(EXPIRE_INTERVAL);
		expireTimer->start();

		statusTimer->setInterval(STATUS_INTERVAL);
		statusTimer->start();

		keepAliveTimer->setInterval(SESSION_EXPIRE / 2);
		keepAliveTimer->start();

		m2KeepAliveTimer->setInterval(M2_KEEPALIVE_INTERVAL);
		m2KeepAliveTimer->start();

		log_info("started");
	}

	bool init(const QHash<QString, QString> &options)
	{
		if(options.contains("version"))
		{
			printf("m2adapter %s\n", VERSION);
			return false;
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
				return false;
			}
		}

		log_info("starting...");

		QString configFile = options.value("config");
		if(configFile.isEmpty())
			configFile = "/etc/m2adapter.conf";

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
		connect(m2_in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(m2_in_readyRead(const QList<QByteArray> &)));

		m2_out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
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
			connect(sock, SIGNAL(readyRead()), SLOT(m2_control_readyRead()));

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
			connect(zhttp_in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(zhttp_in_readyRead(const QList<QByteArray> &)));

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
			connect(zws_in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(zws_in_readyRead(const QList<QByteArray> &)));

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

	void destroySession(Session *s)
	{
		unlinkConnection(s);
		if(s->mode == Http)
			sessionsByZhttpRid.remove(Rid(zhttpInstanceId, s->id));
		else // WebSocket
			sessionsByZwsRid.remove(Rid(zwsInstanceId, s->id));
		delete s;
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

		log_debug("m2: OUT [%s]", buf.data());

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
		m2ConnectionsByRid.remove(Rid(m2_send_idents[conn->identIndex], conn->id));
		delete conn;
	}

	// bodySize = packet.data.size() - framing overhead
	void m2_writeData(M2Connection *conn, const M2ResponsePacket &packet, int bodySize)
	{
		if(conn->flowControl)
		{
			conn->bodyTracker.addPlain(bodySize);
			conn->bodyTracker.specifyEncoded(packet.data.size(), bodySize);

			++(conn->packetsPending);
			conn->packetTracker.addPlain(1);
			conn->packetTracker.specifyEncoded(packet.data.size(), 1);
		}

		m2_out_write(packet);
	}

	void m2_writeOrQueueData(M2Connection *conn, const M2ResponsePacket &packet, int bodySize)
	{
		if(conn->canWrite() && !conn->waitForAllWritten)
		{
			m2_writeData(conn, packet, bodySize);
		}
		else
		{
			M2PendingOutItem *item = 0;
			if(!conn->pendingOutItems.isEmpty())
			{
				M2PendingOutItem &last = conn->pendingOutItems.last();
				if(last.sender == packet.sender && last.id == packet.id)
					item = &last;
			}
			if(!item)
			{
				conn->pendingOutItems += M2PendingOutItem(packet.sender, packet.id);
				item = &(conn->pendingOutItems.last());
			}

			item->data += packet.data;
			item->bodySize += bodySize;
		}
	}

	void m2_tryWriteQueued(M2Connection *conn)
	{
		while(!conn->pendingOutItems.isEmpty() && conn->canWrite())
		{
			M2PendingOutItem item = conn->pendingOutItems.takeFirst();
			M2ResponsePacket packet;
			packet.sender = item.sender;
			packet.id = item.id;
			packet.data = item.data.take();
			m2_writeData(conn, packet, item.bodySize);
		}
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
		m2ConnectionsByRid.remove(Rid(m2_send_idents[conn->identIndex], conn->id));
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

		QByteArray buf = QByteArray("T") + TnetString::fromVariant(packet.toVariant());

		log_debug("%s: OUT %s", logprefix, buf.mid(0, 1000).data());

		if(mode == Http)
			zhttp_out_sock->write(QList<QByteArray>() << buf);
		else // WebSocket
			zws_out_sock->write(QList<QByteArray>() << buf);
	}

	void zhttp_out_write(Mode mode, const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
	{
		const char *logprefix = (mode == Http ? "zhttp" : "zws");

		QByteArray buf = QByteArray("T") + TnetString::fromVariant(packet.toVariant());

		log_debug("%s: OUT instance=%s %s", logprefix, instanceAddress.data(), buf.mid(0, 1000).data());

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
		out.id = s->id;
		out.seq = (s->outSeq)++;
		zhttp_out_write(s->mode, out);
	}

	void zhttp_out_write(Session *s, const ZhttpRequestPacket &packet)
	{
		assert(!s->zhttpAddress.isEmpty());

		ZhttpRequestPacket out = packet;
		out.from = (s->mode == Http ? zhttpInstanceId : zwsInstanceId);
		out.id = s->id;
		out.seq = (s->outSeq)++;
		zhttp_out_write(s->mode, out, s->zhttpAddress);
	}

	void handleControlResponse(int index, const QVariant &data)
	{
#ifdef CONTROL_PORT_DEBUG
		log_debug("m2: IN control %s %s", m2_send_idents[index].data(), qPrintable(TnetString::variantToString(data)));
#endif

		if(data.type() != QVariant::Hash)
			return;

		QVariantHash vhash = data.toHash();

		if(!vhash.contains("rows"))
			return;

		QVariant rows = vhash["rows"];

		// once we get at least one successful response then we flag the port as working
		controlPorts[index].active = true;

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
			if(!conn || !conn->flowControl)
				continue;

			if(bytes_written > conn->confirmedBytesWritten)
			{
				int written = bytes_written - conn->confirmedBytesWritten;
				conn->confirmedBytesWritten = bytes_written;

				handleConnectionBytesWritten(conn, written, true);
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

			m2ConnectionsByRid.remove(Rid(m2_send_idents[conn->identIndex], conn->id));
			delete conn;
		}
	}

	void handleConnectionBytesWritten(M2Connection *conn, int written, bool giveCredits)
	{
		int bodyWritten = conn->bodyTracker.finished(written);
		int packetsWritten = conn->packetTracker.finished(written);

		conn->packetsPending -= packetsWritten;

		if(conn->waitForAllWritten && conn->packetsPending == 0 && conn->pendingOutItems.isEmpty())
			conn->waitForAllWritten = false;

		// if we had any pending writes to make, now's the time
		m2_tryWriteQueued(conn);

		if(conn->session && bodyWritten > 0)
		{
			conn->session->lastActive = time.elapsed();
			handleSessionBodyWritten(conn->session, bodyWritten, giveCredits);
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

		log_debug("%s: IN %s", logprefix, dataRaw.data());

		ZhttpResponsePacket zresp;
		if(!zresp.fromVariant(data))
		{
			log_warning("%s: received message with invalid format (parse failed), skipping", logprefix);
			return;
		}

		Session *s;
		if(mode == Http)
			s = sessionsByZhttpRid.value(Rid(zhttpInstanceId, zresp.id));
		else // WebSocket
			s = sessionsByZwsRid.value(Rid(zwsInstanceId, zresp.id));

		if(!s)
		{
			log_debug("%s: received message for unknown request id, canceling", logprefix);

			// if this was not an error packet, send cancel
			if(!isErrorPacket(zresp) && !zresp.from.isEmpty())
			{
				ZhttpRequestPacket zreq;
				zreq.from = (mode == Http ? zhttpInstanceId : zwsInstanceId);
				zreq.id = zresp.id;
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

				if(zresp.seq != 0)
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
				if(zresp.seq != -1 && zresp.seq != 0)
				{
					log_warning("%s: received response out of sequence (got=%d, expected=-1,0), canceling", logprefix, zresp.seq);

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
			if(zresp.seq != -1 && zresp.seq != s->inSeq)
			{
				log_warning("%s: received response out of sequence (got=%d, expected=%d), canceling", logprefix, zresp.seq, s->inSeq);
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
		if(zresp.seq != -1)
			++(s->inSeq);

		s->lastActive = time.elapsed();

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

		if(s->inHandoff)
		{
			// receiving any message means handoff is complete
			s->inHandoff = false;

			// in order to have been in a handoff state, we would have
			//   had to receive a from address sometime earlier, so it
			//   should be safe to call zhttp_out_write with session.

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
					M2ResponsePacket mresp;
					mresp.sender = m2_send_idents[s->conn->identIndex];
					mresp.id = s->conn->id;

					if(firstDataPacket)
					{
						// use flow control if the control port works and the response is more than one packet
						if(controlPorts[s->conn->identIndex].active && zresp.more)
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

						mresp.data = createResponseHeader(zresp.code, zresp.reason, headers);
					}

					if(!zresp.body.isEmpty())
					{
						if(s->responseHeadersOnly)
						{
							log_warning("%s: received unexpected response body. sending headers only", logprefix);

							bool persistent = s->persistent;

							// send just the headers
							M2Connection *conn = s->conn;
							m2_writeOrQueueData(s->conn, mresp, 0);

							// cancel and destroy session
							if(!s->zhttpAddress.isEmpty())
							{
								ZhttpRequestPacket zreq;
								zreq.type = ZhttpRequestPacket::Cancel;
								zhttp_out_write(s, zreq);
							}
							destroySession(s);
							if(!persistent)
								m2_writeClose(conn);
							return;
						}

						if(s->chunked)
						{
							QByteArray chunkHeader = makeChunkHeader(zresp.body.size());
							QByteArray chunkFooter = makeChunkFooter();

							mresp.data += chunkHeader + zresp.body + chunkFooter;
						}
						else
							mresp.data += zresp.body;
					}

					if(!zresp.more && s->chunked)
					{
						QByteArray chunkHeader = makeChunkHeader(0);
						QByteArray chunkFooter = makeChunkFooter();

						mresp.data += chunkHeader + chunkFooter;
					}

					m2_writeOrQueueData(s->conn, mresp, zresp.body.size());

					if(!s->conn->flowControl)
						handleConnectionBytesWritten(s->conn, mresp.data.size(), zresp.more);
				}
				else
				{
					if(!zresp.more && s->chunked)
					{
						// send closing chunk
						M2ResponsePacket mresp;
						mresp.sender = m2_send_idents[s->conn->identIndex];
						mresp.id = s->conn->id;
						mresp.data = makeChunkHeader(0) + makeChunkFooter();
						m2_writeOrQueueData(s->conn, mresp, 0);
					}
				}

				if(!zresp.more)
				{
					bool persistent = s->persistent;
					M2Connection *conn = s->conn;
					destroySession(s);
					if(!persistent)
						m2_writeClose(conn);
				}
			}
			else // WebSocket
			{
				M2ResponsePacket mresp;
				mresp.sender = m2_send_idents[s->conn->identIndex];
				mresp.id = s->conn->id;

				int payloadSize = 0;

				if(!s->sentResponseHeader)
				{
					s->sentResponseHeader = true;
					s->conn->flowControl = controlPorts[s->conn->identIndex].active;

					HttpHeaders headers = zresp.headers;
					QList<QByteArray> connHeaders = headers.takeAll("Connection");
					foreach(const QByteArray &h, connHeaders)
						headers.removeAll(h);
					headers.removeAll("Transfer-Encoding");
					headers.removeAll("Upgrade");
					headers.removeAll("Sec-Websocket-Accept");

					headers += HttpHeader("Upgrade", "websocket");
					headers += HttpHeader("Connection", "Upgrade");
					headers += HttpHeader("Sec-Websocket-Accept", s->acceptToken);

					QByteArray reason;
					if(!zresp.reason.isEmpty())
						reason = zresp.reason;
					else
						reason = "Switching Protocols";

					mresp.data = createResponseHeader(101, reason, headers);
				}
				else
				{
					int opcode;
					if(zresp.contentType == "binary")
						opcode = 2;
					else // text
						opcode = 1;

					mresp.data = makeWsHeader(!zresp.more, opcode, zresp.body.size()) + zresp.body;

					payloadSize = zresp.body.size();
				}

				m2_writeOrQueueData(s->conn, mresp, payloadSize);

				if(!s->conn->flowControl)
					handleConnectionBytesWritten(s->conn, mresp.data.size(), true);
			}
		}
		else if(zresp.type == ZhttpResponsePacket::Error)
		{
			log_warning("%s: id=%s error condition=%s", logprefix, s->id.data(), zresp.condition.data());

			if(s->mode == WebSocket && zresp.condition == "rejected")
			{
				M2ResponsePacket mresp;
				mresp.sender = m2_send_idents[s->conn->identIndex];
				mresp.id = s->conn->id;

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

				mresp.data = createResponseHeader(zresp.code, zresp.reason, headers);
				mresp.data += zresp.body;

				m2_writeOrQueueData(s->conn, mresp, zresp.body.size());

				M2Connection *conn = s->conn;
				destroySession(s);
				m2_writeClose(conn);
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

			M2ResponsePacket mresp;
			mresp.sender = m2_send_idents[s->conn->identIndex];
			mresp.id = s->conn->id;
			mresp.data = makeWsHeader(true, opcode, 0);

			int payloadSize = 0;

			if(zresp.type == ZhttpResponsePacket::Close)
			{
				QByteArray buf(2, 0);
				writeBigEndian(buf.data(), zresp.code != -1 ? zresp.code : 1000, 2);
				mresp.data += buf;

				payloadSize = buf.size();
			}

			m2_writeOrQueueData(s->conn, mresp, payloadSize);

			if(s->downClosed && s->upClosed)
			{
				M2Connection *conn = s->conn;
				destroySession(s);
				m2_writeClose(conn);
			}
		}
		else
		{
			log_warning("%s: id=%s unsupported type: %d", logprefix, s->id.data(), (int)zresp.type);
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

		log_debug("m2: IN %s", message[0].mid(0, 1000).data());

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

			m2ConnectionsByRid.remove(rid);
			delete conn;

			return;
		}

		Rid m2Rid(mreq.sender, mreq.id);

		M2Connection *conn = m2ConnectionsByRid.value(m2Rid);
		if(!conn)
		{
			if(mreq.version.isEmpty())
			{
				log_error("m2: id=%s no version on initial packet", mreq.id.data());
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

			m2ConnectionsByRid.insert(m2Rid, conn);
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

			int at = host.indexOf(':');
			if(at != -1)
				host = host.mid(0, at);

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

			s = new Session;
			s->conn = conn;
			s->conn->session = s;
			s->lastActive = time.elapsed();
			s->id = m2_send_idents[conn->identIndex] + '_' + conn->id;
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

			if(mreq.type == M2RequestPacket::HttpRequest)
				sessionsByZhttpRid.insert(Rid(zhttpInstanceId, s->id), s);
			else // WebSocketHandshake
				sessionsByZwsRid.insert(Rid(zwsInstanceId, s->id), s);

			log_info("m2: %s id=%s request %s", m2_send_idents[s->conn->identIndex].data(), s->conn->id.data(), uri.toEncoded().data());

			ZhttpRequestPacket zreq;

			zreq.type = ZhttpRequestPacket::Data;
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
					if(mreq.body.size() == 2)
					{
						int hi = (unsigned char)zreq.body[0];
						int lo = (unsigned char)zreq.body[1];
						zreq.code = (hi << 8) + lo;
					}

					s->downClosed = true;
				}
				else if(opcode == 9)
				{
					zreq.type = ZhttpRequestPacket::Ping;
				}
				else // 10
				{
					zreq.type = ZhttpRequestPacket::Pong;
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
						m2_writeClose(conn);
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

			c.state = ControlPort::Idle;
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

	void expire_timeout()
	{
		int now = time.elapsed();
		QList<Session*> toDelete;
		QHashIterator<Rid, Session*> it(sessionsByM2Rid);
		while(it.hasNext())
		{
			it.next();
			Session *s = it.value();
			if(s->lastActive + SESSION_EXPIRE <= now)
				toDelete += s;
		}
		foreach(Session *s, toDelete)
		{
			log_warning("timing out request %s", s->id.data());
			destroySessionAndErrorConnection(s);
		}
	}

	void status_timeout()
	{
		int now = time.elapsed();

		for(int n = 0; n < controlPorts.count(); ++n)
		{
			ControlPort &c = controlPorts[n];

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

	void keepAlive_timeout()
	{
		{
			QHashIterator<Rid, Session*> it(sessionsByZhttpRid);
			while(it.hasNext())
			{
				it.next();
				Session *s = it.value();

				if(!s->inHandoff && !s->zhttpAddress.isEmpty())
				{
					ZhttpRequestPacket zreq;
					zreq.type = ZhttpRequestPacket::KeepAlive;
					zhttp_out_write(s, zreq);
				}
			}
		}

		{
			QHashIterator<Rid, Session*> it(sessionsByZwsRid);
			while(it.hasNext())
			{
				it.next();
				Session *s = it.value();

				if(!s->inHandoff && !s->zhttpAddress.isEmpty())
				{
					ZhttpRequestPacket zreq;
					zreq.type = ZhttpRequestPacket::KeepAlive;
					zhttp_out_write(s, zreq);
				}
			}
		}
	}

	void m2KeepAlive_timeout()
	{
		QHashIterator<Rid, Session*> it(sessionsByM2Rid);
		while(it.hasNext())
		{
			it.next();
			Session *s = it.value();

			QVariantHash args;
			args["keep-alive"] = true;
			m2_writeCtl(s->conn, args);
		}
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
