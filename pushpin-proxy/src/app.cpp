#include <QCoreApplication>
#include <QStringList>
#include <QFile>
#include <QSettings>
#include "qzmqsocket.h"
#include "packet/tnetstring.h"
#include "packet/httprequestpacket.h"
#include "packet/httpresponsepacket.h"
#include "packet/inspectrequestpacket.h"
#include "packet/inspectresponsepacket.h"
#include "app.h"

#define VERSION "1.0"

class App::Private : public QObject
{
	Q_OBJECT

public:
	App *q;
	bool verbose;
	QByteArray clientId;
	QZmq::Socket *m2_in_sock;
	QZmq::Socket *m2https_in_sock;
	QZmq::Socket *m2_out_sock;
	QZmq::Socket *yurl_in_sock;
	QZmq::Socket *yurl_out_sock;
	QZmq::Socket *inspect_req_sock;
	QZmq::Socket *retry_in_sock;
	QZmq::Socket *accept_out_sock;
	int workers;
	int maxWorkers;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		verbose(false),
		m2_in_sock(0),
		m2https_in_sock(0),
		m2_out_sock(0),
		yurl_in_sock(0),
		yurl_out_sock(0),
		inspect_req_sock(0),
		retry_in_sock(0),
		accept_out_sock(0),
		workers(0)
	{
	}

	void log(int level, const char *fmt, va_list ap) const
	{
		if(level <= 1 || verbose)
		{
			QString str;
			str.vsprintf(fmt, ap);

			const char *lstr;
			switch(level)
			{
				case 0: lstr = "ERR"; break;
				case 1: lstr = "WARN"; break;
				case 2:
				default:
					lstr = "INFO"; break;
			}

			fprintf(stderr, "[%s] %s\n", lstr, qPrintable(str));
		}
	}

	void log_info(const char *fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		log(2, fmt, ap);
		va_end(ap);
	}

	void log_warning(const char *fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		log(1, fmt, ap);
		va_end(ap);
	}

	void log_error(const char *fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		log(0, fmt, ap);
		va_end(ap);
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

		if(options.contains("version"))
		{
			printf("pushpin-proxy %s\n", VERSION);
			emit q->quit();
			return;
		}

		if(options.contains("verbose"))
			verbose = true;

		QString configFile = options["config"];
		if(configFile.isEmpty())
			configFile = "/etc/pushpin.conf";

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

		QStringList m2_in_specs = settings.value("proxy/m2_in_specs").toStringList();
		QStringList m2https_in_specs = settings.value("proxy/m2https_in_specs").toStringList();
		QStringList m2_out_specs = settings.value("proxy/m2_out_specs").toStringList();
		QStringList yurl_out_specs = settings.value("proxy/yurl_out_specs").toStringList();
		QStringList yurl_in_specs = settings.value("proxy/yurl_in_specs").toStringList();
		QString inspect_req_spec = settings.value("proxy/inspect_req_spec").toString();
		QString retry_in_spec = settings.value("proxy/retry_in_spec").toString();
		QString accept_out_spec = settings.value("proxy/accept_out_spec").toString();
		maxWorkers = settings.value("proxy/max_open_requests", -1).toInt();

		if(m2_in_specs.isEmpty() && m2https_in_specs.isEmpty())
		{
			log_error("must set one of m2_in_specs or m2https_in_specs");
			emit q->quit();
			return;
		}

		if(m2_out_specs.isEmpty() || yurl_out_specs.isEmpty() || yurl_in_specs.isEmpty())
		{
			log_error("must set m2_out_specs, yurl_out_specs, and yurl_in_specs");
			emit q->quit();
			return;
		}

		if(!m2_in_specs.isEmpty())
		{
			m2_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
			connect(m2_in_sock, SIGNAL(readyRead()), SLOT(m2_in_readyRead()));
			foreach(const QString &url, m2_in_specs)
				m2_in_sock->connectToAddress(url);
		}

		if(!m2https_in_specs.isEmpty())
		{
			m2https_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
			connect(m2https_in_sock, SIGNAL(readyRead()), SLOT(m2https_in_readyRead()));
			foreach(const QString &url, m2https_in_specs)
				m2https_in_sock->connectToAddress(url);
		}

		m2_out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
		connect(m2_out_sock, SIGNAL(messagesWritten(int)), SLOT(m2_out_messagesWritten(int)));
		foreach(const QString &url, m2_out_specs)
			m2_out_sock->connectToAddress(url);

		yurl_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
		connect(yurl_out_sock, SIGNAL(messagesWritten(int)), SLOT(yurl_out_messagesWritten(int)));
		foreach(const QString &url, yurl_out_specs)
			yurl_out_sock->connectToAddress(url);

		clientId = "pushpin-proxy_" + QByteArray::number(QCoreApplication::applicationPid());

		yurl_in_sock = new QZmq::Socket(QZmq::Socket::Sub, this);
		connect(yurl_in_sock, SIGNAL(readyRead()), SLOT(yurl_in_readyRead()));
		foreach(const QString &url, yurl_in_specs)
		{
			yurl_in_sock->subscribe(clientId);
			yurl_in_sock->connectToAddress(url);
		}

		if(!inspect_req_spec.isEmpty())
		{
			inspect_req_sock = new QZmq::Socket(QZmq::Socket::Dealer, this);
			connect(inspect_req_sock, SIGNAL(readyRead()), SLOT(inspect_req_readyRead()));
			connect(inspect_req_sock, SIGNAL(messagesWritten(int)), SLOT(inspect_req_messagesWritten(int)));
			inspect_req_sock->bind(inspect_req_spec);
		}

		if(!retry_in_spec.isEmpty())
		{
			retry_in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
			connect(retry_in_sock, SIGNAL(readyRead()), SLOT(retry_in_readyRead()));
			retry_in_sock->bind(retry_in_spec);
		}

		if(!accept_out_spec.isEmpty())
		{
			accept_out_sock = new QZmq::Socket(QZmq::Socket::Push, this);
			connect(accept_out_sock, SIGNAL(messagesWritten(int)), SLOT(accept_out_messagesWritten(int)));
			accept_out_sock->bind(accept_out_spec);
		}

		log_info("started");
	}

	void handleM2Request(QZmq::Socket *sock, bool https)
	{
		if(maxWorkers != -1 && workers >= maxWorkers)
			return;

		QList<QByteArray> msg = sock->read();
		if(msg.count() != 1)
		{
			log_warning("received message with parts != 1, skipping");
			return;
		}

		log_info("IN m2 %s", qPrintable(TnetString::byteArrayToEscapedString(msg[0])));

		HttpRequestPacket req;
		if(!req.fromM2ByteArray(msg[0]))
		{
			log_warning("received message with invalid format, skipping");
			return;
		}

		req.isHttps = https;

		handleIncomingRequest(req);
	}

	void handleIncomingRequest(const HttpRequestPacket &req)
	{
		printf("id=[%s] method=[%s], path=[%s]\n", req.id.data(), qPrintable(req.method), req.path.data());
		printf("headers:\n");
		foreach(const HttpHeader &header, req.headers)
			printf("  [%s] = [%s]\n", header.first.data(), header.second.data());
		printf("body: [%s]\n", req.body.data());

		HttpResponsePacket resp;
		resp.sender = req.sender;
		resp.id = req.id;
		resp.data = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nok\n";

		m2_out_sock->write(QList<QByteArray>() << resp.toM2ByteArray());
		resp.data = QByteArray();
		m2_out_sock->write(QList<QByteArray>() << resp.toM2ByteArray());
	}

private slots:
	void m2_in_readyRead()
	{
		handleM2Request(m2_in_sock, false);
	}

	void m2https_in_readyRead()
	{
		handleM2Request(m2https_in_sock, true);
	}

	void m2_out_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}

	void yurl_out_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}

	void yurl_in_readyRead()
	{
		// TODO
	}

	void inspect_req_readyRead()
	{
		// TODO
	}

	void inspect_req_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}

	void retry_in_readyRead()
	{
		if(maxWorkers != -1 && workers >= maxWorkers)
			return;

		QList<QByteArray> msg = retry_in_sock->read();
		if(msg.count() != 1)
		{
			log_warning("received message with parts != 1, skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(msg[0], 0, &ok);
		if(!ok)
		{
			log_warning("received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		log_info("IN retry %s", qPrintable(TnetString::variantToString(data)));

		HttpRequestPacket req;
		if(!req.fromVariant(data))
		{
			log_warning("received message with invalid format, skipping");
			return;
		}

		handleIncomingRequest(req);
	}

	void accept_out_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
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
