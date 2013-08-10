/*
 * Copyright (C) 2013 Fanout, Inc.
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

#define VERSION "1.0.0"

#define DEFAULT_HWM 1000
#define EXPIRE_INTERVAL 1000
#define STATUS_INTERVAL 250
#define M2_KEEPALIVE_INTERVAL 90000
#define SESSION_EXPIRE 60000

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

class App::Private : public QObject
{
	Q_OBJECT

public:
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

		ControlPort() :
			sock(0),
			state(Idle),
			active(false)
		{
		}
	};

	// can be used for either m2 or zhttp
	typedef QPair<QByteArray, QByteArray> Rid;

	class Session;

	class M2Connection
	{
	public:
		int identIndex;
		QByteArray id;
		int written;
		int confirmedWritten;
		Session *session;

		M2Connection() :
			written(0),
			confirmedWritten(0),
			session(0)
		{
		}
	};

	class Session
	{
	public:
		int lastActive;

		// m2 stuff
		M2Connection *conn;
		bool persistent;
		bool allowChunked;
		bool respondKeepAlive;
		bool respondClose;
		bool chunked;
		int readCount;
		BufferList pendingIn;
		bool inFinished;

		// zhttp stuff
		QByteArray id;
		QByteArray zhttpAddress;
		bool sentResponseHeader;
		int outSeq;
		int inSeq;
		int credits;
		int pendingInCredits;
		bool inHandoff;

		Session() :
			lastActive(-1),
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
			credits(0),
			pendingInCredits(0),
			inHandoff(false)
		{
		}
	};

	App *q;
	QByteArray instanceId;
	QZmq::Socket *m2_in_sock;
	QZmq::Socket *m2_out_sock;
	QZmq::Socket *zhttp_in_sock;
	QZmq::Socket *zhttp_out_sock;
	QZmq::Socket *zhttp_out_stream_sock;
	QZmq::Valve *m2_in_valve;
	QZmq::Valve *zhttp_in_valve;
	QList<QByteArray> m2_send_idents;
	QHash<Rid, M2Connection*> m2ConnectionsByRid;
	QHash<Rid, Session*> sessionsByM2Rid;
	QHash<Rid, Session*> sessionsByZhttpRid;
	int m2_client_buffer;
	int connectPort;
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
		m2_in_valve(0)
	{
		connect(ProcessQuit::instance(), SIGNAL(quit()), SLOT(doQuit()));

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
		log_startClock();

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
			printf("m2adapter %s\n", VERSION);
			emit q->quit();
			return;
		}

		log_info("starting...");

		if(options.contains("verbose"))
			log_setOutputLevel(LOG_LEVEL_DEBUG);
		else
			log_setOutputLevel(LOG_LEVEL_INFO);

		QString configFile = options["config"];
		if(configFile.isEmpty())
			configFile = "/etc/m2adapter.conf";

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
		m2_client_buffer = settings.value("m2_client_buffer").toInt();
		if(m2_client_buffer <= 0)
			m2_client_buffer = 200000;
		connectPort = settings.value("zhttp_connect_port", -1).toInt();
		ignorePolicies = settings.value("zhttp_ignore_policies").toBool();

		m2_send_idents.clear();
		foreach(const QString &s, str_m2_send_idents)
			m2_send_idents += s.toUtf8();

		if(m2_in_specs.isEmpty() || m2_out_specs.isEmpty() || m2_control_specs.isEmpty())
		{
			log_error("must set m2_in_specs, m2_out_specs, and m2_control_specs");
			emit q->quit();
			return;
		}

		if(m2_send_idents.count() != m2_control_specs.count())
		{
			log_error("m2_control_specs must have the same count as m2_send_idents");
			emit q->quit();
			return;
		}

		if(zhttp_in_specs.isEmpty() || zhttp_out_specs.isEmpty() || zhttp_out_stream_specs.isEmpty())
		{
			log_error("must set zhttp_in_specs, zhttp_out_specs, and zhttp_out_stream_specs");
			emit q->quit();
			return;
		}

		instanceId = "m2adapter_" + QByteArray::number(QCoreApplication::applicationPid());

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
			sock->setHwm(DEFAULT_HWM);
			connect(sock, SIGNAL(readyRead()), SLOT(m2_control_readyRead()));

			log_info("m2_control connect %s:%s", m2_send_idents[n].data(), qPrintable(spec));
			sock->connectToAddress(spec);

			ControlPort controlPort;
			controlPort.sock = sock;
			controlPorts += controlPort;
		}

		zhttp_in_sock = new QZmq::Socket(QZmq::Socket::Sub, this);
		zhttp_in_sock->setHwm(DEFAULT_HWM);
		zhttp_in_sock->subscribe(instanceId + ' ');
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
				emit q->quit();
				return;
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
				emit q->quit();
				return;
			}
		}

		zhttp_out_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);
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
				emit q->quit();
				return;
			}
		}

		m2_in_valve->open();
		zhttp_in_valve->open();

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

	void destroySession(Session *s)
	{
		s->conn->session = 0; // unlink the M2Connection so that it may be reused
		s->conn->confirmedWritten = s->conn->written; // don't notify about existing writes

		sessionsByM2Rid.remove(Rid(m2_send_idents[s->conn->identIndex], s->conn->id));
		sessionsByZhttpRid.remove(Rid(instanceId, s->id));
		delete s;
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

	void zhttp_out_write(const ZhttpRequestPacket &packet)
	{
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(packet.toVariant());

		log_debug("zhttp: OUT %s", buf.mid(0, 1000).data());

		zhttp_out_sock->write(QList<QByteArray>() << buf);
	}

	void zhttp_out_write(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
	{
		QByteArray buf = QByteArray("T") + TnetString::fromVariant(packet.toVariant());

		log_debug("zhttp: OUT %s", buf.mid(0, 1000).data());

		QList<QByteArray> message;
		message += instanceAddress;
		message += QByteArray();
		message += buf;
		zhttp_out_stream_sock->write(message);
	}

	void zhttp_out_writeFirst(Session *s, const ZhttpRequestPacket &packet)
	{
		ZhttpRequestPacket out = packet;
		out.from = instanceId;
		out.id = s->id;
		out.seq = (s->outSeq)++;
		zhttp_out_write(out);
	}

	void zhttp_out_write(Session *s, const ZhttpRequestPacket &packet)
	{
		assert(!s->zhttpAddress.isEmpty());

		ZhttpRequestPacket out = packet;
		out.from = instanceId;
		out.id = s->id;
		out.seq = (s->outSeq)++;
		zhttp_out_write(out, s->zhttpAddress);
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
			if(!conn)
				continue;

			if(bytes_written > conn->confirmedWritten)
			{
				int written = bytes_written - conn->confirmedWritten;
				conn->confirmedWritten = bytes_written;

				if(conn->session)
				{
					conn->session->lastActive = time.elapsed();

					// note: if the session finishes for any reason before
					handleResponseWritten(conn->session, written, true, true);
				}
			}
		}

		// any connections missing?
		QList<M2Connection*> gone;
		QHashIterator<Rid, M2Connection*> it(m2ConnectionsByRid);
		while(it.hasNext())
		{
			it.next();
			M2Connection *conn = it.value();
			if(conn->identIndex == index && !ids.contains(conn->id))
				gone += conn;
		}
		foreach(M2Connection *conn, gone)
		{
			log_debug("m2: %s id=%s disconnected", m2_send_idents[conn->identIndex].data(), conn->id.data());

			if(conn->session)
			{
				// if a worker had ack'd this session, then send error
				if(!conn->session->zhttpAddress.isEmpty())
				{
					ZhttpRequestPacket zreq;
					zreq.type = ZhttpRequestPacket::Error;
					zreq.condition = "disconnected";
					zhttp_out_write(conn->session, zreq);
				}

				destroySession(conn->session);
			}

			m2ConnectionsByRid.remove(Rid(m2_send_idents[conn->identIndex], conn->id));
			delete conn;
		}
	}

	void handleResponseWritten(Session *s, int written, bool flowControl, bool giveCredits)
	{
		s->pendingInCredits += written;

		log_debug("request id=%s written %d%s", s->id.data(), written, flowControl ? "" : " (no flow control)");

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

		if(mreq.isDisconnect)
		{
			log_debug("m2: %s id=%s disconnected", mreq.sender.data(), mreq.id.data());

			Rid rid(mreq.sender, mreq.id);

			M2Connection *conn = m2ConnectionsByRid.value(rid);
			if(!conn)
				return;

			if(conn->session)
			{
				// if a worker had ack'd this session, then send cancel
				if(!conn->session->zhttpAddress.isEmpty())
				{
					ZhttpRequestPacket zreq;
					zreq.type = ZhttpRequestPacket::Cancel;
					zhttp_out_write(conn->session, zreq);
				}

				destroySession(conn->session);
			}

			m2ConnectionsByRid.remove(rid);
			delete conn;

			return;
		}

		bool more = false;
		if(mreq.uploadStreamOffset >= 0 && !mreq.uploadStreamDone)
			more = true;

		Rid m2Rid(mreq.sender, mreq.id);

		Session *s = 0;

		M2Connection *conn = m2ConnectionsByRid.value(m2Rid);
		if(!conn)
		{
			if(mreq.version != "HTTP/1.0" && mreq.version != "HTTP/1.1")
			{
				log_error("m2: id=%s skipping unknown version: %s", mreq.id.data(), mreq.version.data());
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

			if(mreq.uploadStreamOffset > 0)
			{
				log_warning("m2: id=%s stream offset > 0 but session unknown", mreq.id.data());
				m2_writeCtlCancel(mreq.sender, mreq.id);
				return;
			}

			if(sessionsByM2Rid.contains(m2Rid))
			{
				log_warning("m2: received duplicate request id=%s, skipping", mreq.id.data());
				m2_writeErrorClose(mreq.sender, mreq.id);
				return;
			}

			conn = new M2Connection;
			conn->identIndex = index;
			conn->id = mreq.id;

			m2ConnectionsByRid.insert(m2Rid, conn);
		}
		else
		{
			s = sessionsByM2Rid.value(m2Rid);

			if(!s && mreq.uploadStreamOffset > 0)
			{
				log_warning("m2: id=%s stream offset > 0 but session unknown", mreq.id.data());
				destroySession(s);
				m2_writeCtlCancel(conn);
				return;
			}
		}

		if(!s)
		{
			QByteArray scheme;
			if(mreq.scheme == "https")
				scheme = "https";
			else
				scheme = "http";

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
			QUrl uri = QUrl::fromEncoded(uriRaw, QUrl::StrictMode);
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

			if(!more)
				s->inFinished = true;

			sessionsByM2Rid.insert(m2Rid, s);
			sessionsByZhttpRid.insert(Rid(instanceId, s->id), s);

			log_info("m2: %s id=%s request %s", m2_send_idents[s->conn->identIndex].data(), s->conn->id.data(), uri.toEncoded().data());

			ZhttpRequestPacket zreq;
			zreq.type = ZhttpRequestPacket::Data;
			zreq.credits = m2_client_buffer;
			zreq.stream = true;
			zreq.method = mreq.method;
			zreq.uri = uri;
			zreq.headers = mreq.headers;
			zreq.body = mreq.body;
			zreq.more = !s->inFinished;
			zreq.peerAddress = mreq.remoteAddress;
			if(connectPort != -1)
				zreq.connectPort = connectPort;
			if(ignorePolicies)
				zreq.ignorePolicies = true;
			zhttp_out_writeFirst(s, zreq);
		}
		else
		{
			int offset = 0;
			if(mreq.uploadStreamOffset > 0)
				offset = mreq.uploadStreamOffset;

			if(offset != s->readCount)
			{
				log_warning("m2: %s id=%s unexpected stream offset (got=%d, expected=%d)", m2_send_idents[s->conn->identIndex].data(), mreq.id.data(), offset, s->readCount);
				M2Connection *conn = s->conn;
				destroySession(s);
				m2_writeCtlCancel(conn);
				return;
			}

			if(s->zhttpAddress.isEmpty())
			{
				log_error("m2: %s id=%s multiple packets from m2 before response from zhttp", m2_send_idents[s->conn->identIndex].data(), mreq.id.data());
				M2Connection *conn = s->conn;
				destroySession(s);
				m2_writeCtlCancel(conn);
				return;
			}

			s->readCount += mreq.body.size();

			if(!more)
				s->inFinished = true;

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

			if(controlPorts[index].state != ControlPort::ExpectingResponse)
			{
				log_warning("m2: received unexpected control response, skipping");
				continue;
			}

			handleControlResponse(index, data);

			controlPorts[index].state = ControlPort::Idle;
		}
	}

	void zhttp_in_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("zhttp: received message with parts != 1, skipping");
			return;
		}

		int at = message[0].indexOf(' ');
		if(at == -1)
		{
			log_warning("zhttp: received message with invalid format, skipping");
			return;
		}

		QByteArray dataRaw = message[0].mid(at + 1);
		if(dataRaw.length() < 1 || dataRaw[0] != 'T')
		{
			log_warning("zhttp: received message with invalid format (missing type), skipping");
			return;
		}

		QVariant data = TnetString::toVariant(dataRaw.mid(1));
		if(data.isNull())
		{
			log_warning("zhttp: received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		log_debug("zhttp: IN %s", dataRaw.data());

		ZhttpResponsePacket zresp;
		if(!zresp.fromVariant(data))
		{
			log_warning("zhttp: received message with invalid format (parse failed), skipping");
			return;
		}

		Session *s = sessionsByZhttpRid.value(Rid(instanceId, zresp.id));
		if(!s)
		{
			log_debug("zhttp: received message for unknown request id, canceling");

			// if this was not an error packet, send cancel
			if(!isErrorPacket(zresp) && !zresp.from.isEmpty())
			{
				ZhttpRequestPacket zreq;
				zreq.from = instanceId;
				zreq.id = zresp.id;
				zreq.type = ZhttpRequestPacket::Cancel;
				zhttp_out_write(zreq, zresp.from);
			}

			return;
		}

		if(s->inSeq == 0)
		{
			// are we expecting a sequence of packets after the first?
			if((!isErrorPacket(zresp) && zresp.type != ZhttpResponsePacket::Data) || (zresp.type == ZhttpResponsePacket::Data && zresp.more))
			{
				// sequence must have from address
				if(zresp.from.isEmpty())
				{
					log_warning("zhttp: received first response of sequence with no from address, canceling");
					destroySession(s);
					return;
				}

				s->zhttpAddress = zresp.from;

				if(zresp.seq != 0)
				{
					log_warning("zhttp: received first response of sequence without valid seq, canceling");
					ZhttpRequestPacket zreq;
					zreq.type = ZhttpRequestPacket::Cancel;
					zhttp_out_write(s, zreq);
					destroySession(s);
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
					log_warning("zhttp: received response out of sequence (got=%d, expected=-1,0), canceling", zresp.seq);

					if(!s->zhttpAddress.isEmpty())
					{
						ZhttpRequestPacket zreq;
						zreq.type = ZhttpRequestPacket::Cancel;
						zhttp_out_write(s, zreq);
					}

					destroySession(s);
					return;
				}
			}

			zresp.seq = 0;
		}
		else
		{
			if(zresp.seq == -1)
			{
				// sender is trying his luck
				zresp.seq = s->inSeq;
			}
			else if(zresp.seq != s->inSeq)
			{
				log_warning("zhttp: received response out of sequence (got=%d, expected=%d), canceling", zresp.seq, s->inSeq);
				ZhttpRequestPacket zreq;
				zreq.type = ZhttpRequestPacket::Cancel;
				zhttp_out_write(s, zreq);
				destroySession(s);
				return;
			}

			// if a new from address is provided, update our copy
			if(!zresp.from.isEmpty())
				s->zhttpAddress = zresp.from;
		}

		assert(zresp.seq >= 0);
		++(s->inSeq);

		s->lastActive = time.elapsed();

		if(s->inHandoff)
		{
			// receiving any message means handoff is complete
			s->inHandoff = false;

			// in order to have been in a handoff state, we would have
			//   had to receive a from address sometime earlier
			assert(!s->zhttpAddress.isEmpty());

			if(!s->pendingIn.isEmpty())
			{
				ZhttpRequestPacket zreq;
				zreq.type = ZhttpRequestPacket::Data;

				// send credits too, if needed (though this probably can't happen?)
				if(s->pendingInCredits > 0)
				{
					zreq.credits = s->pendingInCredits;
					s->pendingInCredits = 0;
				}

				zreq.body = s->pendingIn.take();
				zreq.more = !s->inFinished;
				zhttp_out_write(s, zreq);
			}
			else if(s->pendingInCredits > 0)
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

			bool firstDataPacket = !s->sentResponseHeader;

			// respond with data if we have body data or this is the first packet
			if(!zresp.body.isEmpty() || firstDataPacket)
			{
				M2ResponsePacket mresp;
				mresp.sender = m2_send_idents[s->conn->identIndex];
				mresp.id = s->conn->id;

				int overhead = 0;

				if(firstDataPacket)
				{
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

					if(s->chunked)
					{
						connHeaders += "Transfer-Encoding";
						headers += HttpHeader("Transfer-Encoding", "chunked");
					}
					else if(!zresp.more && !headers.contains("Content-Length"))
					{
						headers += HttpHeader("Content-Length", QByteArray::number(zresp.body.size()));
					}

					if(!connHeaders.isEmpty())
						headers += HttpHeader("Connection", HttpHeaders::join(connHeaders));

					mresp.data = createResponseHeader(zresp.code, zresp.reason, headers);

					overhead += mresp.data.size();
				}

				if(s->chunked)
				{
					QByteArray chunkHeader = makeChunkHeader(zresp.body.size());
					QByteArray chunkFooter = makeChunkFooter();

					mresp.data += chunkHeader + zresp.body + chunkFooter;
					overhead += chunkHeader.size() + chunkFooter.size();

					if(!zresp.more)
					{
						chunkHeader = makeChunkHeader(0);

						mresp.data += chunkHeader + chunkFooter;
						overhead += chunkHeader.size() + chunkFooter.size();
					}
				}
				else
					mresp.data += zresp.body;

				m2_out_write(mresp);

				s->conn->written += overhead + zresp.body.size();
				s->conn->confirmedWritten += overhead;

				if((firstDataPacket && !zresp.more) || (!controlPorts[s->conn->identIndex].active && zresp.body.size() > 0))
				{
					// no outbound flow control
					int written = zresp.body.size();
					s->conn->confirmedWritten += written;

					handleResponseWritten(s, written, false, zresp.more);
				}
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
					m2_out_write(mresp);
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
		else if(zresp.type == ZhttpResponsePacket::Error)
		{
			log_warning("zhttp: id=%s error condition=%s", s->id.data(), zresp.condition.data());
			M2Connection *conn = s->conn;
			destroySession(s);
			m2_writeErrorClose(conn);
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
			M2Connection *conn = s->conn;
			destroySession(s);
			m2_writeErrorClose(conn);
		}
		else if(zresp.type == ZhttpResponsePacket::HandoffStart)
		{
			s->inHandoff = true;

			ZhttpRequestPacket zreq;
			zreq.type = ZhttpRequestPacket::HandoffProceed;
			zhttp_out_write(s, zreq);
		}
		else
		{
			log_warning("zhttp: id=%s unsupported type: %d", s->id.data(), (int)zresp.type);
		}
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
			M2Connection *conn = s->conn;
			destroySession(s);
			m2_writeErrorClose(conn);
		}
	}

	void status_timeout()
	{
		for(int n = 0; n < controlPorts.count(); ++n)
		{
			if(controlPorts[n].state == ControlPort::Idle)
			{
				// query m2 for connection info (to track bytes written)
				QVariantHash cmdArgs;
				cmdArgs["what"] = QByteArray("net");
				controlPorts[n].state = ControlPort::ExpectingResponse;
				m2_control_write(n, "status", cmdArgs);
			}
		}
	}

	void keepAlive_timeout()
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
