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
#include "log.h"

#define VERSION "1.0.0"

#define DEFAULT_HWM 1000
#define EXPIRE_INTERVAL 1000
#define STATUS_INTERVAL 250
#define SESSION_EXPIRE 60000

//#define CONTROL_PORT_DEBUG

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

/*static QByteArray createResponse(int code, const QByteArray &reason, const HttpHeaders &_headers, const QByteArray &body)
{
	HttpHeaders headers = _headers;
	if(!headers.contains("Content-Length"))
		headers += HttpHeader("Content-Length", QByteArray::number(body.size()));
	return createResponseHeader(code, reason, headers) + body;
}*/

class App::Private : public QObject
{
	Q_OBJECT

public:
	enum ControlState
	{
		ControlIdle,
		ControlExpectingResponse
	};

	class Session
	{
	public:
		// we use the same id on both sides of the adapter
		//   m2 provides a unique request id for its instance
		//   zhttp expects a request id unique to our instance to reply to
		QByteArray id;
		int lastActive;

		// m2 stuff
		QByteArray httpVersion;
		bool persistent;
		bool allowChunked;
		bool respondKeepAlive;
		bool respondClose;
		bool chunked;
		int offset;
		int written;
		int confirmedWritten;
		bool inFinished;

		// zhttp stuff
		QByteArray zhttpAddress;
		int outSeq;
		int inSeq;
		int credits;

		Session() :
			lastActive(-1),
			persistent(false),
			allowChunked(false),
			respondKeepAlive(false),
			respondClose(false),
			chunked(false),
			offset(0),
			written(0),
			confirmedWritten(0),
			inFinished(false),
			outSeq(0),
			inSeq(0),
			credits(0)
		{
		}
	};

	App *q;
	QZmq::Socket *m2_in_sock;
	QZmq::Socket *m2_out_sock;
	QZmq::Socket *m2_control_sock;
	QZmq::Socket *zhttp_in_sock;
	QZmq::Socket *zhttp_out_sock;
	QZmq::Socket *zhttp_out_stream_sock;
	QZmq::Valve *m2_in_valve;
	QZmq::Valve *zhttp_in_valve;
	QHash<QByteArray, Session*> sessionsById;
	QByteArray m2_out_ident;
	int m2_client_buffer;
	bool ignorePolicies;
	ControlState controlState;
	QTime time;
	QTimer *expireTimer;
	QTimer *statusTimer;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		m2_in_sock(0),
		m2_out_sock(0),
		m2_control_sock(0),
		zhttp_in_sock(0),
		zhttp_out_sock(0),
		zhttp_out_stream_sock(0),
		m2_in_valve(0),
		controlState(ControlIdle)
	{
		connect(ProcessQuit::instance(), SIGNAL(quit()), SLOT(doQuit()));

		time.start();

		expireTimer = new QTimer(this);
		connect(expireTimer, SIGNAL(timeout()), SLOT(expire_timeout()));

		statusTimer = new QTimer(this);
		connect(statusTimer, SIGNAL(timeout()), SLOT(status_timeout()));
	}

	~Private()
	{
		QHashIterator<QByteArray, Session*> it(sessionsById);
		while(it.hasNext())
		{
			it.next();
			delete it.value();
		}
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

		QString m2_in_spec = settings.value("m2_in_spec").toString();
		QString m2_out_spec = settings.value("m2_out_spec").toString();
		m2_out_ident = settings.value("m2_out_ident").toString().toUtf8();
		QString m2_control_spec = settings.value("m2_control_spec").toString();
		bool zhttp_connect = settings.value("zhttp_connect").toBool();
		QString zhttp_in_spec = settings.value("zhttp_in_spec").toString();
		QString zhttp_out_spec = settings.value("zhttp_out_spec").toString();
		QString zhttp_out_stream_spec = settings.value("zhttp_out_stream_spec").toString();
		m2_client_buffer = settings.value("m2_client_buffer").toInt();
		if(m2_client_buffer <= 0)
			m2_client_buffer = 200000;
		ignorePolicies = settings.value("zhttp_ignore_policies").toBool();

		m2_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
		m2_in_sock->setHwm(DEFAULT_HWM);
		m2_in_sock->connectToAddress(m2_in_spec);

		m2_in_valve = new QZmq::Valve(m2_in_sock, this);
		connect(m2_in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(m2_in_readyRead(const QList<QByteArray> &)));

		m2_out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
		m2_out_sock->setHwm(DEFAULT_HWM);
		m2_out_sock->setWriteQueueEnabled(false);
		m2_out_sock->connectToAddress(m2_out_spec);

		m2_control_sock = new QZmq::Socket(QZmq::Socket::Dealer, this);
		m2_control_sock->setShutdownWaitTime(0);
		m2_control_sock->setHwm(DEFAULT_HWM);
		connect(m2_control_sock, SIGNAL(readyRead()), SLOT(m2_control_readyRead()));
		m2_control_sock->connectToAddress(m2_control_spec);

		zhttp_in_sock = new QZmq::Socket(QZmq::Socket::Sub, this);
		zhttp_in_sock->setHwm(DEFAULT_HWM);
		zhttp_in_sock->subscribe(m2_out_ident + ' ');
		if(zhttp_connect)
		{
			zhttp_in_sock->connectToAddress(zhttp_in_spec);
		}
		else
		{
			if(!zhttp_in_sock->bind(zhttp_in_spec))
			{
				log_error("unable to bind to zhttp_in_spec: %s", qPrintable(zhttp_in_spec));
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
			zhttp_out_sock->connectToAddress(zhttp_out_spec);
		}
		else
		{
			if(!zhttp_out_sock->bind(zhttp_out_spec))
			{
				log_error("unable to bind to zhttp_out_spec: %s", qPrintable(zhttp_out_spec));
				emit q->quit();
				return;
			}
		}

		zhttp_out_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);
		zhttp_out_stream_sock->setHwm(DEFAULT_HWM);
		if(zhttp_connect)
		{
			zhttp_out_stream_sock->connectToAddress(zhttp_out_stream_spec);
		}
		else
		{
			if(!zhttp_out_stream_sock->bind(zhttp_out_stream_spec))
			{
				log_error("unable to bind to zhttp_out_stream_spec: %s", qPrintable(zhttp_out_stream_spec));
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

		log_info("started");
	}

	void m2_out_write(const M2ResponsePacket &packet)
	{
		QByteArray buf = packet.toByteArray();

		log_debug("m2: OUT [%s]", buf.data());

		m2_out_sock->write(QList<QByteArray>() << buf);
	}

	void m2_control_write(const QByteArray &cmd, const QVariantHash &args)
	{
		QVariantList vlist;
		vlist += cmd;
		vlist += args;

		QByteArray buf = TnetString::fromVariant(vlist);

#ifdef CONTROL_PORT_DEBUG
		log_debug("m2: OUT control %s", buf.data());
#endif

		controlState = ControlExpectingResponse;

		QList<QByteArray> message;
		message += QByteArray();
		message += buf;
		m2_control_sock->write(message);
	}

	void m2_writeErrorClose(Session *s)
	{
		M2ResponsePacket mresp;
		mresp.sender = m2_out_ident;
		mresp.id = s->id;
		mresp.data = "";
		m2_out_write(mresp);
	}

	void zhttp_out_write(const ZhttpRequestPacket &packet)
	{
		QByteArray buf = TnetString::fromVariant(packet.toVariant());

		log_debug("zhttp: OUT %s", buf.data());

		zhttp_out_sock->write(QList<QByteArray>() << buf);
	}

	void zhttp_out_write(const ZhttpRequestPacket &packet, const QByteArray &instanceAddress)
	{
		QByteArray buf = TnetString::fromVariant(packet.toVariant());

		log_debug("zhttp: OUT %s", buf.data());

		QList<QByteArray> message;
		message += instanceAddress;
		message += QByteArray();
		message += buf;
		zhttp_out_stream_sock->write(message);
	}

	void handleControlResponse(const QVariant &data)
	{
#ifdef CONTROL_PORT_DEBUG
		log_debug("m2: IN control %s", qPrintable(TnetString::variantToString(data)));
#endif

		QVariantHash vhash = data.toHash();
		QVariant rows = vhash["rows"];
		foreach(const QVariant &row, rows.toList())
		{
			QVariantList vlist = row.toList();
			QByteArray id = vlist[0].toByteArray();
			int written = vlist[7].toInt();

			Session *s = sessionsById.value(id);
			if(!s)
				continue;

			if(written > s->confirmedWritten)
			{
				int x = written - s->confirmedWritten;
				s->confirmedWritten = written;
				handleResponseWritten(s, x);
			}
		}
	}

	void handleResponseWritten(Session *s, int written)
	{
		log_debug("request id=%s written %d/%d", s->id.data(), s->confirmedWritten, s->written);

		s->lastActive = time.elapsed();

		if(!s->zhttpAddress.isEmpty())
		{
			ZhttpRequestPacket zreq;
			zreq.from = m2_out_ident;
			zreq.id = s->id;
			zreq.seq = (s->outSeq)++;
			zreq.type = ZhttpRequestPacket::Credit;
			zreq.credits = written;
			zhttp_out_write(zreq, s->zhttpAddress);
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

		M2RequestPacket mreq;
		if(!mreq.fromByteArray(message[0]))
		{
			log_warning("m2: received message with invalid format, skipping");
			return;
		}

		if(mreq.isDisconnect)
		{
			log_debug("m2: id=%s disconnected", mreq.id.data());

			Session *s = sessionsById.value(mreq.id);
			if(s)
			{
				// if a worker had ack'd this session, then send cancel
				if(!s->zhttpAddress.isEmpty())
				{
					ZhttpRequestPacket zreq;
					zreq.from = m2_out_ident;
					zreq.id = s->id;
					zreq.type = ZhttpRequestPacket::Cancel;
					zreq.seq = (s->outSeq)++;
					zhttp_out_write(zreq, s->zhttpAddress);
				}

				sessionsById.remove(mreq.id);
				delete s;
			}

			return;
		}

		// TODO: handle upload stream packets

		QByteArray uri;

		if(mreq.scheme == "https")
			uri += "https://";
		else
			uri += "http://";

		QByteArray host = mreq.headers.get("Host");
		if(host.isEmpty())
			host = "localhost";

		int at = host.indexOf(':');
		if(at != -1)
			host = host.mid(0, at);

		if(!validateHost(host))
		{
			log_warning("m2: invalid host [%s], skipping", host.data());
			return;
		}

		if(!mreq.uri.startsWith('/'))
		{
			log_warning("m2: invalid uri [%s], skipping", mreq.uri.data());
			return;
		}

		uri += host;
		uri += mreq.uri;

		Session *s = sessionsById.value(mreq.id);
		if(s)
		{
			log_warning("m2: received duplicate request id=%s, skipping", mreq.id.data());
			return;
		}
		else
		{
			if(mreq.version != "HTTP/1.0" && mreq.version != "HTTP/1.1")
			{
				log_warning("m2: id=%s skipping unknown version: %s", mreq.id.data(), mreq.version.data());
				return;
			}

			s = new Session;
			s->id = mreq.id;
			s->lastActive = time.elapsed();
			s->httpVersion = mreq.version;

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

			// TODO: if input is streamed, then we wouldn't set this yet
			s->inFinished = true;

			sessionsById.insert(mreq.id, s);

			log_info("m2: id=%s request %s", s->id.data(), uri.data());

			ZhttpRequestPacket zreq;
			zreq.from = m2_out_ident;
			zreq.id = s->id;
			zreq.type = ZhttpRequestPacket::Data;
			zreq.seq = (s->outSeq)++;
			zreq.credits = m2_client_buffer;
			zreq.stream = true;
			zreq.method = mreq.method;
			zreq.uri = QUrl::fromEncoded(uri, QUrl::StrictMode);
			if(ignorePolicies)
				zreq.ignorePolicies = true;
			zhttp_out_write(zreq);
		}
	}

	void m2_control_readyRead()
	{
		while(m2_control_sock->canRead())
		{
			QList<QByteArray> message = m2_control_sock->read();

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

			if(controlState != ControlExpectingResponse)
			{
				log_warning("m2: received unexpected control response, skipping");
				continue;
			}

			handleControlResponse(data);

			controlState = ControlIdle;
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
		QVariant data = TnetString::toVariant(dataRaw);
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

		Session *s = sessionsById.value(zresp.id);
		if(!s)
		{
			log_debug("zhttp: received message for unknown request id, canceling");

			// if this was not an error packet, send cancel
			if(zresp.type != ZhttpResponsePacket::Error && zresp.type != ZhttpResponsePacket::Cancel && !zresp.from.isEmpty())
			{
				ZhttpRequestPacket zreq;
				zreq.from = m2_out_ident;
				zreq.id = zresp.id;
				zreq.seq = (s->outSeq)++;
				zreq.type = ZhttpRequestPacket::Cancel;
				zhttp_out_write(zreq, zresp.from);
			}

			return;
		}

		// no from address is okay as long as we'd never need to communicate back, not even to give credits
		if(s->zhttpAddress.isEmpty() && zresp.from.isEmpty() && (!s->inFinished || zresp.more))
		{
			log_warning("zhttp: received first response with no from address, canceling");
			sessionsById.remove(s->id);
			delete s;
			return;
		}

		if(!zresp.from.isEmpty())
			s->zhttpAddress = zresp.from;

		s->lastActive = time.elapsed();

		if(zresp.type == ZhttpResponsePacket::Data)
		{
			log_debug("zhttp: id=%s response data size=%d%s", s->id.data(), zresp.body.size(), zresp.more ? " M" : "");

			M2ResponsePacket mresp;
			mresp.sender = m2_out_ident;
			mresp.id = s->id;

			int overhead = 0;

			// for the first response message, we need to include http headers
			if(s->written == 0)
			{
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
			}
			else
				mresp.data += zresp.body;

			m2_out_write(mresp);

			s->written += overhead + zresp.body.size();
			s->confirmedWritten += overhead;

			if(!zresp.more)
			{
				if(s->chunked)
				{
					// send closing chunk
					M2ResponsePacket mresp;
					mresp.sender = m2_out_ident;
					mresp.id = s->id;
					mresp.data = makeChunkHeader(0) + makeChunkFooter();
					m2_out_write(mresp);
				}

				if(!s->persistent)
				{
					// close
					M2ResponsePacket mresp;
					mresp.sender = m2_out_ident;
					mresp.id = s->id;
					mresp.data = "";
					m2_out_write(mresp);
				}

				sessionsById.remove(s->id);
				delete s;
			}
		}
		else if(zresp.type == ZhttpResponsePacket::Error)
		{
			log_warning("zhttp: id=%s error condition=%s", s->id.data(), zresp.condition.data());
			m2_writeErrorClose(s);
			sessionsById.remove(s->id);
			delete s;
		}
		else if(zresp.type == ZhttpResponsePacket::Credit)
		{
			// TODO: care about this once we have streaming input
		}
		else if(zresp.type == ZhttpResponsePacket::Cancel)
		{
			m2_writeErrorClose(s);
			sessionsById.remove(s->id);
			delete s;
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
		QHashIterator<QByteArray, Session*> it(sessionsById);
		while(it.hasNext())
		{
			it.next();
			Session *s = it.value();
			if(s->lastActive + SESSION_EXPIRE <= now)
				toDelete += s;
		}
		for(int n = 0; n < toDelete.count(); ++n)
		{
			Session *s = toDelete[n];
			log_warning("timing out request %s", s->id.data());
			m2_writeErrorClose(s);
			sessionsById.remove(s->id);
			delete s;
		}
	}

	void status_timeout()
	{
		if(controlState == ControlIdle)
		{
			// query m2 for connection info (to track bytes written)
			QVariantHash cmdArgs;
			cmdArgs["what"] = QByteArray("net");
			controlState = ControlExpectingResponse;
			m2_control_write("status", cmdArgs);
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
